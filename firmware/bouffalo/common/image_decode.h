// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns caller-owned BGRA8888 pixels in PSRAM.
uint8_t* mpx_decode_image(const uint8_t* bytes, uint32_t length, int* width, int* height);

#ifdef __cplusplus
}
#endif
