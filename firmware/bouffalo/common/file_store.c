// SPDX-License-Identifier: Apache-2.0
#include "file_store.h"

#include <stdio.h>

#include "lfs_port.h"

static char partition[] = "mp_kv";
static struct lfs_context context = {.partition_name = partition};
static struct lfs_config config = {
    .read_size = 256,
    .prog_size = 256,
    .block_size = 4096,
    .block_cycles = 500,
    .cache_size = 512,
    .lookahead_size = 16,
};
static lfs_t* store;

bool mpx_files_mount(void) {
    if (!store) store = lfs_xip_init(&context, &config);
    return store != NULL;
}

int mpx_file_read(const char* name, void* out, size_t capacity, size_t* length) {
    *length = 0;
    if (!store) return LFS_ERR_IO;
    lfs_file_t file;
    int result = lfs_file_open(store, &file, name, LFS_O_RDONLY);
    if (result) return result;
    const lfs_soff_t size = lfs_file_size(store, &file);
    if (size < 0) {
        result = size;
    } else {
        *length = size;
        if ((size_t)size > capacity)
            result = LFS_ERR_NOSPC;
        else if (size && lfs_file_read(store, &file, out, size) != size)
            result = LFS_ERR_IO;
    }
    const int closed = lfs_file_close(store, &file);
    return result ? result : closed;
}

int mpx_file_write(const char* name, const void* data, size_t length) {
    if (!store) return LFS_ERR_IO;
    char temporary[32];
    const int characters = snprintf(temporary, sizeof(temporary), "%s.tmp", name);
    if (characters < 0 || (size_t)characters >= sizeof(temporary)) return LFS_ERR_INVAL;
    lfs_file_t file;
    const int opened = lfs_file_open(store, &file, temporary, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (opened) return opened;
    const lfs_ssize_t written = lfs_file_write(store, &file, data, length);
    const int closed = lfs_file_close(store, &file);
    if (written < 0) return written;
    if ((size_t)written != length) return LFS_ERR_IO;
    return closed ? closed : lfs_rename(store, temporary, name);
}

int mpx_file_remove(const char* name) { return store ? lfs_remove(store, name) : LFS_ERR_IO; }
