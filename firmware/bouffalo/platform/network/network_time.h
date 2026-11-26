// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t monotonic_ms;
    uint32_t seconds;
    uint32_t microseconds;
} mpx_network_time_sync_t;

void mpx_network_time_start(void);
bool mpx_network_time_ready(void);
bool mpx_network_time_take_sync(mpx_network_time_sync_t* sync);
void mpx_network_set_time(uint32_t seconds, uint32_t microseconds);
void mpx_network_get_time(uint32_t* seconds, uint32_t* microseconds);

#ifdef __cplusplus
}
#endif
