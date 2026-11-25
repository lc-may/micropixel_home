// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool mpx_files_mount(void);
int mpx_file_read(const char* name, void* out, size_t capacity, size_t* length);
int mpx_file_write(const char* name, const void* data, size_t length);
int mpx_file_remove(const char* name);

#ifdef __cplusplus
}
#endif
