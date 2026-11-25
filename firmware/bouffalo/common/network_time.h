// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mpx_network_time_start(void);
bool mpx_network_time_ready(void);
void mpx_network_set_time(uint32_t seconds);
void mpx_network_get_time(uint32_t* seconds, uint32_t* microseconds);

#ifdef __cplusplus
}
#endif
