// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool mpx_app_store_mount(void);
struct lfs* mpx_app_store_filesystem(void);
bool mpx_app_store_visit(bool (*visitor)(void*, const char*), void* context);
size_t mpx_app_store_file_size(const char* id);
int mpx_app_store_read(const char* id, void* header, size_t header_size, void* packed, size_t capacity, size_t* length);
int mpx_app_store_commit(const char* id, const void* header, size_t header_size, const void* packed, size_t length);
int mpx_app_store_remove(const char* id);
size_t mpx_app_store_free_bytes(void);

#ifdef __cplusplus
}
#endif
