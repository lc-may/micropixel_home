// SPDX-License-Identifier: Apache-2.0
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lfs.h"

static uint8_t* flash;

static uint8_t* address(const struct lfs_config* config, lfs_block_t block, lfs_off_t offset) {
    return flash + (size_t)block * config->block_size + offset;
}

static int read_block(const struct lfs_config* config, lfs_block_t block, lfs_off_t offset, void* bytes,
                      lfs_size_t size) {
    memcpy(bytes, address(config, block, offset), size);
    return 0;
}

static int program_block(const struct lfs_config* config, lfs_block_t block, lfs_off_t offset, const void* bytes,
                         lfs_size_t size) {
    uint8_t* destination = address(config, block, offset);
    const uint8_t* source = bytes;
    for (lfs_size_t i = 0; i < size; ++i) {
        if ((destination[i] & source[i]) != source[i]) return LFS_ERR_IO;
        destination[i] &= source[i];
    }
    return 0;
}

static int erase_block(const struct lfs_config* config, lfs_block_t block) {
    memset(address(config, block, 0), UINT8_MAX, config->block_size);
    return 0;
}

static int sync_flash(const struct lfs_config* config) {
    (void)config;
    return 0;
}

static bool add_file(lfs_t* store, const char* path) {
    const char* name = strrchr(path, '/');
    FILE* input = fopen(path, "rb");
    if (!input) return false;
    lfs_file_t file;
    if (lfs_file_open(store, &file, name ? name + 1 : path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL) != 0) {
        fclose(input);
        return false;
    }
    uint8_t bytes[4096];
    size_t count;
    bool success = true;
    while ((count = fread(bytes, 1, sizeof(bytes), input)) != 0) {
        if (lfs_file_write(store, &file, bytes, count) != (lfs_ssize_t)count) {
            success = false;
            break;
        }
    }
    success = success && !ferror(input);
    fclose(input);
    return lfs_file_close(store, &file) == 0 && success;
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    char* end;
    const unsigned long size = strtoul(argv[2], &end, 10);
    const lfs_size_t block_size = 4096;
    if (*end || size < 2 * block_size || size > UINT32_MAX || size % block_size) return 2;
    flash = malloc(size);
    if (!flash) return 1;
    memset(flash, UINT8_MAX, size);
    const struct lfs_config config = {.read = read_block,
                                      .prog = program_block,
                                      .erase = erase_block,
                                      .sync = sync_flash,
                                      .read_size = 256,
                                      .prog_size = 256,
                                      .block_size = block_size,
                                      .block_count = size / block_size,
                                      .block_cycles = 500,
                                      .cache_size = 512,
                                      .lookahead_size = 32};
    lfs_t store;
    bool success = lfs_format(&store, &config) == 0 && lfs_mount(&store, &config) == 0;
    if (success) {
        for (int i = 3; success && i < argc; ++i) {
            success = add_file(&store, argv[i]);
            if (!success) fprintf(stderr, "Could not add %s to the Orion App Store\n", argv[i]);
        }
        success = lfs_unmount(&store) == 0 && success;
    }
    if (success) {
        FILE* output = fopen(argv[1], "wb");
        if (output) {
            success = fwrite(flash, 1, size, output) == size;
            success = fclose(output) == 0 && success;
        } else {
            success = false;
        }
    }
    free(flash);
    return success ? 0 : 1;
}
