// SPDX-License-Identifier: Apache-2.0
#include "FreeRTOS.h"
#include "mm.h"

void *pvPortMalloc(size_t bytes) {
    void *memory = kmalloc(bytes, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_16BYTE);
    traceMALLOC(memory, bytes);
    return memory;
}

void vPortFree(void *memory) {
    if (!memory) return;
    kfree(memory);
    traceFREE(memory, 0);
}

void *pvPortMallocStack(size_t bytes) { return kmalloc(bytes, MM_FLAG_HEAP_OCRAM_0 | MM_FLAG_ALIGN_16BYTE); }

void vPortFreeStack(void *memory) { kfree(memory); }

#if defined(MICROPIXEL_ORION_WIRELESS_HEAP)
void *orion_wireless_malloc(size_t bytes, uint32_t flags) {
    return kmalloc(bytes, (flags & ~MM_FLAG_HEAP_MASK) | MM_FLAG_HEAP_WRAM_0);
}
#endif
