// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string.h>

#include "mm.h"

/* krealloc has no heap or alignment flags. Keep the eight-byte alignment
 * required by the runtime even when the SDK would move the allocation. */
static inline void *mpx_psram_realloc(void *memory, size_t size) {
    if (size == 0) {
        kfree(memory);
        return NULL;
    }
    void *replacement = kmalloc(size, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE);
    if (replacement && memory) {
        const size_t old_size = kmalloc_size(memory);
        memcpy(replacement, memory, old_size < size ? old_size : size);
        kfree(memory);
    }
    return replacement;
}
