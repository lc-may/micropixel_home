// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

#include "esp_heap_caps.h"

namespace micropixel::work {

inline void* AllocatePsram(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
inline void* AllocateZeroedPsram(size_t size) {
    return heap_caps_calloc(1U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
inline void* AllocatePsramAligned64(size_t size) {
    return heap_caps_aligned_alloc(64U, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
inline void FreePsram(void* memory) { heap_caps_free(memory); }

}  // namespace micropixel::work
