// SPDX-License-Identifier: Apache-2.0
#include "app_store_flash.c"

#include <assert.h>
#include <stdio.h>

#include "file_store.h"

enum { kBase = 0x400000, kCapacity = 0x400000, kTypical = 200 * 1024, kUpgrade = 768 * 1024 };
enum { kKvBase = 0x3c9000, kKvCapacity = 0x20000 };
static uint8_t flash[kCapacity], snapshot[kCapacity], payload[kUpgrade], received[kUpgrade];
static uint8_t kv[kKvCapacity];
static unsigned writes_until_failure;
static bool injecting_failure;

int bflb_boot2_partition_addr_active(const char* name, uint32_t* address, uint32_t* size) {
    assert(strcmp(name, "mp_apps") == 0 || strcmp(name, "mp_kv") == 0);
    const bool app = !strcmp(name, "mp_apps");
    *address = app ? kBase : kKvBase;
    *size = app ? kCapacity : kKvCapacity;
    return 0;
}

static uint8_t* bytes_at(uint32_t address, uint32_t length) {
    if (address < kBase) {
        assert(address >= kKvBase && address + length <= kKvBase + kKvCapacity);
        return kv + address - kKvBase;
    }
    assert(address + length <= kBase + kCapacity);
    return flash + address - kBase;
}

int bflb_flash_read(uint32_t address, uint8_t* out, uint32_t length) {
    memcpy(out, bytes_at(address, length), length);
    return 0;
}

static uint32_t block_address(const struct lfs_config* cfg, lfs_block_t block) {
    const struct lfs_context* fs = cfg->context;
    return fs->flash_addr + block * cfg->block_size;
}

int lfs_xip_flash_read(const struct lfs_config* cfg, lfs_block_t block, lfs_off_t offset, void* out,
                       lfs_size_t length) {
    return bflb_flash_read(block_address(cfg, block) + offset, out, length);
}

static bool power_cut(void) {
    if (!injecting_failure) return false;
    if (!writes_until_failure) return true;
    --writes_until_failure;
    return false;
}

int lfs_xip_flash_prog(const struct lfs_config* cfg, lfs_block_t block, lfs_off_t offset, const void* data,
                       lfs_size_t length) {
    if (power_cut()) return LFS_ERR_IO;
    uint8_t* destination = bytes_at(block_address(cfg, block) + offset, length);
    const uint8_t* source = data;
    for (size_t byte = 0; byte < length; ++byte) {
        assert((destination[byte] & source[byte]) == source[byte]);
        destination[byte] &= source[byte];
    }
    return 0;
}

int lfs_xip_flash_erase(const struct lfs_config* cfg, lfs_block_t block) {
    if (power_cut()) return LFS_ERR_IO;
    memset(bytes_at(block_address(cfg, block), cfg->block_size), UINT8_MAX, cfg->block_size);
    return 0;
}

int lfs_xip_flash_sync(const struct lfs_config* cfg) {
    (void)cfg;
    return 0;
}

static void reboot(void) {
    assert(lfs_unmount(&context.lfs) == 0);
    mounted = false;
    assert(mpx_app_store_mount());
}

static bool count_app(void* count, const char* name) {
    assert(strncmp(name, "app", 3) == 0 || strncmp(name, "micropixel.", 11) == 0);
    ++*(unsigned*)count;
    return true;
}

static unsigned count_apps(void) {
    unsigned count = 0;
    assert(mpx_app_store_visit(count_app, &count));
    return count;
}

static void verify(const char* id, uint32_t version, size_t size) {
    size_t length = 0;
    uint32_t header = 0;
    assert(mpx_app_store_read(id, &header, sizeof(header), received, sizeof(received), &length) == 0);
    assert(version == header && size == length);
    for (size_t i = 0; i < length; ++i) assert(received[i] == version);
}

static void verify_data(const char* id, const char* key, uint32_t expected) {
    uint32_t value = 0;
    size_t size = 0;
    assert(mpx_app_data_read(id, key, NULL, 0, &size) == LFS_ERR_NOSPC && size == sizeof(value));
    assert(mpx_app_data_read(id, key, &value, sizeof(value), &size) == 0 && size == sizeof(value));
    assert(value == expected);
}

static void verify_app_data(void) {
    const uint32_t value = 41, newer = 42;
    const char* weather = "micropixel.weather";
    const char* music = "micropixel.netease-music";
    memset(kv, UINT8_MAX, sizeof(kv));
    assert(mpx_files_mount());
    assert(mpx_file_write("settings.wifi", &value, sizeof(value)) == 0);
    assert(mpx_file_write("weather.v1", &value, sizeof(value)) == 0);
    assert(mpx_file_write("clock.notes", &value, sizeof(value)) == 0);
    assert(mpx_file_write("clock.alarms.v1", &value, sizeof(value)) == 0);
    assert(mpx_file_write("ncm.auth", &value, sizeof(value)) == 0);
    injecting_failure = true;
    writes_until_failure = 0;
    assert(!mpx_app_data_migrate());
    injecting_failure = false;
    uint32_t out = 0;
    size_t size = 0;
    assert(mpx_file_read("weather.v1", &out, sizeof(out), &size) == 0 && out == value);
    reboot();
    assert(mpx_app_data_migrate());
    verify_data(weather, "weather.v1", value);
    verify_data(MPX_CLOCK_APP_ID, "notes.done", value);
    verify_data(MPX_CLOCK_APP_ID, MPX_CLOCK_ALARMS_KEY, value);
    verify_data(music, "ncm.auth", value);
    assert(mpx_file_read("weather.v1", &out, sizeof(out), &size) == LFS_ERR_NOENT);
    assert(mpx_file_read("settings.wifi", &out, sizeof(out), &size) == 0 && out == value);

    assert(mpx_app_data_remove(music, "ncm.auth") == 0);
    reboot();
    assert(mpx_app_data_migrate());
    assert(mpx_app_data_read(music, "ncm.auth", &out, sizeof(out), &size) == LFS_ERR_NOENT);
    assert(mpx_app_data_read(music, "weather.v1", &out, sizeof(out), &size) == LFS_ERR_NOENT);
    assert(mpx_app_data_read(music, "../weather.v1", &out, sizeof(out), &size) == LFS_ERR_INVAL);
    assert(mpx_app_data_ensure("../../PSM") == LFS_ERR_INVAL);
    assert(mpx_app_data_write(music, MPX_CLOCK_ALARMS_KEY, &value, sizeof(value)) == LFS_ERR_INVAL);

    assert(mpx_app_data_write("app00", "config", &value, sizeof(value)) == 0);
    assert(mpx_app_data_write("app01", "config", &newer, sizeof(newer)) == 0);
    assert(mpx_app_store_remove("app00") == 0);
    assert(mpx_app_store_commit("app00", &newer, sizeof(newer), payload, kTypical) == 0);
    reboot();
    verify_data("app00", "config", value);
    verify_data("app01", "config", newer);
    injecting_failure = true;
    writes_until_failure = 0;
    assert(mpx_app_data_write("app00", "config", &newer, sizeof(newer)) < 0);
    injecting_failure = false;
    reboot();
    verify_data("app00", "config", value);
    for (unsigned i = 0; i < 16; ++i) {
        char key[12];
        snprintf(key, sizeof(key), "data%u", i);
        assert(mpx_app_data_write("quota", key, payload, 1024) == 0);
    }
    assert(mpx_app_data_write("quota", "overflow", &value, sizeof(value)) == LFS_ERR_NOSPC);
    assert(mpx_app_data_write("quota", "data0", payload, 1025) == LFS_ERR_NOSPC);
    assert(mpx_app_data_write("quota", "data0", &value, sizeof(value)) == 0);
}

static void verify_factory(const char* path) {
    FILE* image = fopen(path, "rb");
    assert(image);
    assert(fread(flash, 1, sizeof(flash), image) == sizeof(flash));
    assert(fgetc(image) == EOF);
    fclose(image);
    assert(mpx_app_store_mount());
    static const char* const defaults[] = {"micropixel.settings", "micropixel.clock-alarm",
                                           "micropixel.weather",  "micropixel.netease-music",
                                           "micropixel.news",     "micropixel.bluetooth-speaker"};
    const size_t factory_count = sizeof(defaults) / sizeof(*defaults);
    assert(count_apps() == factory_count);
    for (size_t i = 0; i < factory_count; ++i) assert(mpx_app_store_file_size(defaults[i]) > 0);
    uint8_t header[8];
    size_t size = 0;
    assert(mpx_app_store_read("micropixel.netease-music", header, sizeof(header), received, sizeof(received), &size) ==
           0);
    assert(size > 0 && memcmp(header, "MPXZ", 4) == 0 &&
           memcmp(received,
                  "\xfd"
                  "7zXZ\0",
                  6) == 0);
    const uint32_t replacement = 3;
    assert(mpx_app_store_commit("micropixel.netease-music", &replacement, sizeof(replacement), payload, kTypical) == 0);
    reboot();
    assert(count_apps() == factory_count);
    assert(mpx_app_store_remove("micropixel.netease-music") == 0);
    reboot();
    assert(count_apps() == factory_count - 1);
}

int main(int argc, char** argv) {
    if (argc == 2) {
        verify_factory(argv[1]);
        return 0;
    }
    memset(flash, UINT8_MAX, sizeof(flash));
    flash[kCapacity - 1] = 0;
    assert(!mpx_app_store_mount());
    assert(flash[kCapacity - 1] == 0);
    flash[kCapacity - 1] = UINT8_MAX;
    assert(mpx_app_store_mount());

    const uint32_t old_version = 1;
    memset(payload, old_version, sizeof(payload));
    for (unsigned i = 0; i < 15; ++i) {
        char id[12];
        snprintf(id, sizeof(id), "app%02u", i);
        assert(mpx_app_data_ensure(id) == 0);
        assert(mpx_app_store_commit(id, &old_version, sizeof(old_version), payload, kTypical) == 0);
    }
    assert(count_apps() == 15);
    assert(mpx_app_store_free_bytes() >= kUpgrade);
    verify("app00", old_version, kTypical);
    memcpy(snapshot, flash, sizeof(snapshot));

    const uint32_t new_version = 2;
    memset(payload, new_version, sizeof(payload));
    const unsigned interruptions[] = {0, 2, 80, 800};
    for (size_t i = 0; i < sizeof(interruptions) / sizeof(*interruptions); ++i) {
        writes_until_failure = interruptions[i];
        injecting_failure = true;
        assert(mpx_app_store_commit("app00", &new_version, sizeof(new_version), payload, kUpgrade) < 0);
        injecting_failure = false;
        reboot();
        verify("app00", old_version, kTypical);
        assert(count_apps() == 15);
        assert(lfs_unmount(&context.lfs) == 0);
        mounted = false;
        memcpy(flash, snapshot, sizeof(flash));
        assert(mpx_app_store_mount());
    }

    assert(mpx_app_store_commit("app00", &new_version, sizeof(new_version), payload, kUpgrade) == 0);
    reboot();
    verify("app00", new_version, kUpgrade);
    verify_app_data();
    assert(count_apps() == 15);
    assert(mpx_app_store_remove("app00") == 0);
    reboot();
    assert(mpx_app_store_file_size("app00") == 0);
    assert(count_apps() == 14);
    return 0;
}
