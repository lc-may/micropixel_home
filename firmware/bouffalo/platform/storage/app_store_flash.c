// SPDX-License-Identifier: Apache-2.0
#include "app_store_flash.h"

#include <string.h>

#include "bflb_boot2.h"
#include "bflb_flash.h"
#include "lfs_port.h"

static struct lfs_context context;
static bool mounted;
#if defined(CONFIG_FREERTOS)
static StaticSemaphore_t lock_storage;
#endif
static const char transaction[] = ".upload";
static struct lfs_config config = {
    .context = &context,
    .read = lfs_xip_flash_read,
    .prog = lfs_xip_flash_prog,
    .erase = lfs_xip_flash_erase,
    .sync = lfs_xip_flash_sync,
    .read_size = 256,
    .prog_size = 256,
    .block_size = 4096,
    .block_cycles = 500,
    .cache_size = 512,
    .lookahead_size = 32,
};

#if defined(LFS_THREADSAFE)
static int store_lock(const struct lfs_config* cfg) {
#if defined(CONFIG_FREERTOS)
    struct lfs_context* fs = cfg->context;
    xSemaphoreTakeRecursive(fs->fs_giant_lock, portMAX_DELAY);
#else
    (void)cfg;
#endif
    return 0;
}

static int store_unlock(const struct lfs_config* cfg) {
#if defined(CONFIG_FREERTOS)
    struct lfs_context* fs = cfg->context;
    xSemaphoreGiveRecursive(fs->fs_giant_lock);
#else
    (void)cfg;
#endif
    return 0;
}
#endif

static bool erased(uint32_t address, uint32_t size) {
    uint8_t bytes[256];
    for (uint32_t offset = 0; offset < size; offset += sizeof(bytes)) {
        if (bflb_flash_read(address + offset, bytes, sizeof(bytes)) != 0) return false;
        for (size_t byte = 0; byte < sizeof(bytes); ++byte) {
            if (bytes[byte] != UINT8_MAX) return false;
        }
    }
    return true;
}

bool mpx_app_store_mount(void) {
    if (mounted) return true;
    uint32_t size;
    if (bflb_boot2_partition_addr_active("mp_apps", &context.flash_addr, &size) != 0 ||
        context.flash_addr != 0x400000 || size != 0x400000)
        return false;
    config.block_count = size / config.block_size;
#if defined(LFS_THREADSAFE)
    config.lock = store_lock;
    config.unlock = store_unlock;
#endif
#if defined(CONFIG_FREERTOS)
    if (!context.fs_giant_lock) context.fs_giant_lock = xSemaphoreCreateRecursiveMutexStatic(&lock_storage);
    if (!context.fs_giant_lock) return false;
#endif
    int result = lfs_mount(&context.lfs, &config);
    if (result == LFS_ERR_CORRUPT && erased(context.flash_addr, size) && lfs_format(&context.lfs, &config) == 0)
        result = lfs_mount(&context.lfs, &config);
    if (result != 0) return false;
    mounted = true;
    (void)lfs_remove(&context.lfs, transaction);
    return true;
}

struct lfs* mpx_app_store_filesystem(void) { return mounted ? &context.lfs : NULL; }

bool mpx_app_store_visit(bool (*visitor)(void*, const char*), void* arg) {
    lfs_dir_t directory;
    if (!mounted || lfs_dir_open(&context.lfs, &directory, "/") != 0) return false;
    struct lfs_info info;
    int result;
    while ((result = lfs_dir_read(&context.lfs, &directory, &info)) > 0) {
        if (info.type == LFS_TYPE_REG && info.name[0] != '.' && !visitor(arg, info.name)) break;
    }
    const int closed = lfs_dir_close(&context.lfs, &directory);
    return result >= 0 && closed == 0;
}

size_t mpx_app_store_file_size(const char* id) {
    struct lfs_info info;
    return mounted && lfs_stat(&context.lfs, id, &info) == 0 && info.type == LFS_TYPE_REG ? info.size : 0;
}

int mpx_app_store_read(const char* id, void* header, size_t header_size, void* packed, size_t capacity,
                       size_t* length) {
    if (!mounted) return LFS_ERR_IO;
    lfs_file_t file;
    int result = lfs_file_open(&context.lfs, &file, id, LFS_O_RDONLY);
    if (result != 0) return result;
    const lfs_soff_t size = lfs_file_size(&context.lfs, &file);
    if (size < 0) {
        result = size;
    } else if ((size_t)size <= header_size || (size_t)size - header_size > capacity) {
        result = LFS_ERR_INVAL;
    } else {
        *length = (size_t)size - header_size;
        if (lfs_file_read(&context.lfs, &file, header, header_size) != (lfs_ssize_t)header_size ||
            lfs_file_read(&context.lfs, &file, packed, *length) != (lfs_ssize_t)*length)
            result = LFS_ERR_IO;
    }
    const int closed = lfs_file_close(&context.lfs, &file);
    return result ? result : closed;
}

int mpx_app_store_commit(const char* id, const void* header, size_t header_size, const void* packed, size_t length) {
    if (!mounted) return LFS_ERR_IO;
    lfs_file_t file;
    int result = lfs_file_open(&context.lfs, &file, transaction, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (result != 0) return result;
    const lfs_ssize_t head = lfs_file_write(&context.lfs, &file, header, header_size);
    const lfs_ssize_t body =
        head == (lfs_ssize_t)header_size ? lfs_file_write(&context.lfs, &file, packed, length) : head;
    const int closed = lfs_file_close(&context.lfs, &file);
    if (head < 0 || body < 0)
        result = head < 0 ? head : body;
    else if (head != (lfs_ssize_t)header_size || body != (lfs_ssize_t)length)
        result = LFS_ERR_IO;
    else
        result = closed ? closed : lfs_rename(&context.lfs, transaction, id);
    if (result != 0) (void)lfs_remove(&context.lfs, transaction);
    return result;
}

int mpx_app_store_remove(const char* id) { return mounted ? lfs_remove(&context.lfs, id) : LFS_ERR_IO; }

size_t mpx_app_store_free_bytes(void) {
    if (!mounted) return 0;
    const lfs_ssize_t blocks = lfs_fs_size(&context.lfs);
    return blocks < 0 || (lfs_size_t)blocks >= config.block_count ? 0
                                                                  : (config.block_count - blocks) * config.block_size;
}
