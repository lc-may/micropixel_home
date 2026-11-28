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

// Run before Guest tasks start. In AOT mode the App Store must already be mounted.
bool mpx_app_data_migrate(void);
int mpx_app_data_ensure(const char* app_id);
int mpx_app_data_read(const char* app_id, const char* key, void* out, size_t capacity, size_t* length);
int mpx_app_data_write(const char* app_id, const char* key, const void* data, size_t length);
int mpx_app_data_remove(const char* app_id, const char* key);

#define MPX_CLOCK_APP_ID "micropixel.clock-alarm"
#define MPX_CLOCK_ALARMS_KEY "@alarms.v1"

#ifdef __cplusplus
}
#endif
