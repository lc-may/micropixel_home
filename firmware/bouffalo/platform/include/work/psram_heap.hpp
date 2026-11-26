// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstring>

#include "mm.h"

namespace micropixel::work {

inline void* AllocatePsram(size_t size) { return kmalloc(size, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE); }
inline void* AllocateZeroedPsram(size_t size) {
    void* memory = AllocatePsram(size);
    // Clear the buffer after kmalloc releases the SDK heap lock.
    if (memory) std::memset(memory, 0, size);
    return memory;
}
inline void* AllocatePsramAligned64(size_t size) { return kmalloc(size, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_64BYTE); }
inline void FreePsram(void* memory) { kfree(memory); }

}  // namespace micropixel::work
