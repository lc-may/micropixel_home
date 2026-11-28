// SPDX-License-Identifier: Apache-2.0
#include "file_store.h"

#include <stdio.h>
#include <string.h>

#include "abi/micropixel_abi.h"
#include "bflb_boot2.h"
#include "lfs_port.h"
#include "runtime/bundle/bundle_format.h"
#if WASM_ENABLE_AOT
#include "app_store_flash.h"
#endif

#if defined(LFS_THREADSAFE)
static StaticSemaphore_t lock_storage;

static int store_lock(const struct lfs_config* config) {
    struct lfs_context* context = config->context;
    xSemaphoreTakeRecursive(context->fs_giant_lock, portMAX_DELAY);
    return 0;
}

static int store_unlock(const struct lfs_config* config) {
    struct lfs_context* context = config->context;
    xSemaphoreGiveRecursive(context->fs_giant_lock);
    return 0;
}
#endif

static struct lfs_context context;
static struct lfs_config config = {
    .context = &context,
    .read = lfs_xip_flash_read,
    .prog = lfs_xip_flash_prog,
    .erase = lfs_xip_flash_erase,
    .sync = lfs_xip_flash_sync,
#if defined(LFS_THREADSAFE)
    .lock = store_lock,
    .unlock = store_unlock,
#endif
    .read_size = 256,
    .prog_size = 256,
    .block_size = 4096,
    .block_cycles = 500,
    .cache_size = 512,
    .lookahead_size = 16,
};
static lfs_t* store;

bool mpx_files_mount(void) {
    if (store) return true;

    // The SDK block callbacks use physical Flash offsets. The XIP lookup
    // rejects partitions below FW slot B, including Orion's relocated mp_kv.
    uint32_t size;
    if (bflb_boot2_partition_addr_active("mp_kv", &context.flash_addr, &size) != 0 || !context.flash_addr ||
        context.flash_addr % config.block_size || !size || size % config.block_size)
        return false;
    config.block_count = size / config.block_size;
#if defined(LFS_THREADSAFE)
    if (!context.fs_giant_lock) context.fs_giant_lock = xSemaphoreCreateRecursiveMutexStatic(&lock_storage);
    if (!context.fs_giant_lock) return false;
#endif
    int error = lfs_mount(&context.lfs, &config);
    if (error == LFS_ERR_CORRUPT && lfs_format(&context.lfs, &config) == 0) error = lfs_mount(&context.lfs, &config);
    if (error) return false;

    store = &context.lfs;
    return true;
}

static int read_file(lfs_t* fs, const char* name, void* out, size_t capacity, size_t* length) {
    *length = 0;
    if (!fs) return LFS_ERR_IO;
    lfs_file_t file;
    int result = lfs_file_open(fs, &file, name, LFS_O_RDONLY);
    if (result) return result;
    const lfs_soff_t size = lfs_file_size(fs, &file);
    if (size < 0) {
        result = size;
    } else {
        *length = size;
        if ((size_t)size > capacity)
            result = LFS_ERR_NOSPC;
        else if (size && lfs_file_read(fs, &file, out, size) != size)
            result = LFS_ERR_IO;
    }
    const int closed = lfs_file_close(fs, &file);
    return result ? result : closed;
}

static int write_file(lfs_t* fs, const char* name, const char* temporary, const void* data, size_t length) {
    lfs_file_t file;
    int result = lfs_file_open(fs, &file, temporary, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (result) return result;
    const lfs_ssize_t written = lfs_file_write(fs, &file, data, length);
    const int closed = lfs_file_close(fs, &file);
    if (written < 0)
        result = written;
    else if ((size_t)written != length)
        result = LFS_ERR_IO;
    else
        result = closed ? closed : lfs_rename(fs, temporary, name);
    if (result) (void)lfs_remove(fs, temporary);
    return result;
}

int mpx_file_read(const char* name, void* out, size_t capacity, size_t* length) {
    return read_file(store, name, out, capacity, length);
}

int mpx_file_write(const char* name, const void* data, size_t length) {
    if (!store) return LFS_ERR_IO;
    char temporary[32];
    const int characters = snprintf(temporary, sizeof(temporary), "%s.tmp", name);
    if (characters < 0 || (size_t)characters >= sizeof(temporary)) return LFS_ERR_INVAL;
    return write_file(store, name, temporary, data, length);
}

static const char app_data_root[] = "/.data";
static const char temporary_key[] = "@tmp";
enum {
    APP_DIRECTORY_BYTES = sizeof(app_data_root) + 1 + MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH,
    APP_FILE_BYTES = APP_DIRECTORY_BYTES + 1 + MICROPIXEL_STORAGE_MAX_KEY_BYTES,
    APP_MAX_KEYS = 16,
    APP_MAX_BYTES = 16 * 1024,
};

static lfs_t* app_data_filesystem(void) {
#if WASM_ENABLE_AOT
    return mpx_app_store_filesystem();
#else
    return store;
#endif
}

static bool identifier(const char* value, size_t maximum) {
    if (!value || !*value || !strcmp(value, ".") || !strcmp(value, "..")) return false;
    for (size_t i = 0; value[i]; ++i) {
        const char c = value[i];
        if (i >= maximum || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                              c == '_' || c == '.'))
            return false;
    }
    return true;
}

static bool app_directory(char out[APP_DIRECTORY_BYTES], const char* app_id) {
    if (!identifier(app_id, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) || app_id[0] == '.') return false;
    snprintf(out, APP_DIRECTORY_BYTES, "%s/%s", app_data_root, app_id);
    return true;
}

static void app_file(char out[APP_FILE_BYTES], const char* directory, const char* key) {
    snprintf(out, APP_FILE_BYTES, "%s/%s", directory, key);
}

static bool app_key(const char* app_id, const char* key) {
    return identifier(key, MICROPIXEL_STORAGE_MAX_KEY_BYTES) ||
           (key && !strcmp(key, MPX_CLOCK_ALARMS_KEY) && !strcmp(app_id, MPX_CLOCK_APP_ID));
}

static int ensure_directory(lfs_t* fs, const char* path) {
    const int error = lfs_mkdir(fs, path);
    if (error != LFS_ERR_EXIST) return error;
    struct lfs_info info;
    const int result = lfs_stat(fs, path, &info);
    return result ? result : info.type == LFS_TYPE_DIR ? 0 : LFS_ERR_NOTDIR;
}

static int ensure_app(lfs_t* fs, const char* directory) {
    const int result = ensure_directory(fs, app_data_root);
    return result ? result : ensure_directory(fs, directory);
}

int mpx_app_data_ensure(const char* app_id) {
    char directory[APP_DIRECTORY_BYTES];
    if (!app_directory(directory, app_id)) return LFS_ERR_INVAL;
    lfs_t* fs = app_data_filesystem();
    return fs ? ensure_app(fs, directory) : LFS_ERR_IO;
}

int mpx_app_data_read(const char* app_id, const char* key, void* out, size_t capacity, size_t* length) {
    char directory[APP_DIRECTORY_BYTES], path[APP_FILE_BYTES];
    if (!app_directory(directory, app_id) || !app_key(app_id, key) || (!out && capacity)) return LFS_ERR_INVAL;
    app_file(path, directory, key);
    return read_file(app_data_filesystem(), path, out, capacity, length);
}

static int check_quota(lfs_t* fs, const char* directory, const char* key, size_t length) {
    lfs_dir_t dir;
    int result = lfs_dir_open(fs, &dir, directory);
    if (result) return result;
    struct lfs_info info;
    size_t bytes = length, keys = 1;
    while ((result = lfs_dir_read(fs, &dir, &info)) > 0) {
        if (info.type == LFS_TYPE_REG && info.name[0] != '@' && strcmp(info.name, key)) {
            bytes += info.size;
            ++keys;
        }
    }
    const int closed = lfs_dir_close(fs, &dir);
    if (result < 0 || closed) return result < 0 ? result : closed;
    return bytes <= APP_MAX_BYTES && keys <= APP_MAX_KEYS ? 0 : LFS_ERR_NOSPC;
}

int mpx_app_data_write(const char* app_id, const char* key, const void* data, size_t length) {
    char directory[APP_DIRECTORY_BYTES], path[APP_FILE_BYTES], temporary[APP_FILE_BYTES];
    if (!app_directory(directory, app_id) || !app_key(app_id, key) || (!data && length) ||
        length > MICROPIXEL_STORAGE_MAX_VALUE_BYTES)
        return LFS_ERR_INVAL;
    lfs_t* fs = app_data_filesystem();
    if (!fs) return LFS_ERR_IO;
    app_file(path, directory, key);
    app_file(temporary, directory, temporary_key);
#if defined(LFS_THREADSAFE)
    fs->cfg->lock(fs->cfg);
#endif
    int result = ensure_app(fs, directory);
    if (!result && key[0] != '@') result = check_quota(fs, directory, key, length);
    if (!result) result = write_file(fs, path, temporary, data, length);
#if defined(LFS_THREADSAFE)
    fs->cfg->unlock(fs->cfg);
#endif
    return result;
}

int mpx_app_data_remove(const char* app_id, const char* key) {
    char directory[APP_DIRECTORY_BYTES], path[APP_FILE_BYTES];
    if (!app_directory(directory, app_id) || !app_key(app_id, key)) return LFS_ERR_INVAL;
    app_file(path, directory, key);
    lfs_t* fs = app_data_filesystem();
    return fs ? lfs_remove(fs, path) : LFS_ERR_IO;
}

static int import_file(lfs_t* fs, const char* name, const char* legacy, const char* temporary) {
    lfs_file_t source, destination;
    int result = lfs_file_open(store, &source, legacy, LFS_O_RDONLY);
    if (result) return result;
    result = lfs_file_open(fs, &destination, temporary, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (!result) {
        uint8_t buffer[256];
        lfs_ssize_t read;
        while ((read = lfs_file_read(store, &source, buffer, sizeof(buffer))) > 0) {
            const lfs_ssize_t written = lfs_file_write(fs, &destination, buffer, read);
            if (written != read) {
                result = written < 0 ? written : LFS_ERR_IO;
                break;
            }
        }
        if (read < 0) result = read;
        const int closed = lfs_file_close(fs, &destination);
        if (!result) result = closed ? closed : lfs_rename(fs, temporary, name);
    }
    const int closed = lfs_file_close(store, &source);
    if (result) (void)lfs_remove(fs, temporary);
    return result ? result : closed;
}

bool mpx_app_data_migrate(void) {
    static const struct {
        const char *app_id, *key, *legacy;
    } records[] = {
        {"micropixel.weather", "weather.v1", "weather.v1"},
        {MPX_CLOCK_APP_ID, MPX_CLOCK_ALARMS_KEY, "clock.alarms.v1"},
        {MPX_CLOCK_APP_ID, "notes.done", "clock.notes"},
        {"micropixel.netease-music", "ncm.auth", "ncm.auth"},
    };
    if (!mpx_files_mount()) return false;
    lfs_t* fs = app_data_filesystem();
    if (!fs) return false;
    bool success = true;
    for (size_t i = 0; i < sizeof(records) / sizeof(*records); ++i) {
        struct lfs_info info;
        int result = lfs_stat(store, records[i].legacy, &info);
        if (result == LFS_ERR_NOENT) continue;
        if (result || info.type != LFS_TYPE_REG || info.size > MICROPIXEL_STORAGE_MAX_VALUE_BYTES) {
            success = false;
            continue;
        }
        char directory[APP_DIRECTORY_BYTES], path[APP_FILE_BYTES], temporary[APP_FILE_BYTES];
        if (!app_directory(directory, records[i].app_id)) return false;
        app_file(path, directory, records[i].key);
        app_file(temporary, directory, temporary_key);
        result = lfs_stat(fs, path, &info);
        if (result == LFS_ERR_NOENT) {
            result = ensure_app(fs, directory);
            if (!result) result = import_file(fs, path, records[i].legacy, temporary);
        } else if (!result && info.type != LFS_TYPE_REG) {
            result = LFS_ERR_INVAL;
        }
        if (result || lfs_remove(store, records[i].legacy)) success = false;
    }
    return success;
}
