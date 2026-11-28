// SPDX-License-Identifier: Apache-2.0
#include "embedded_xz.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mm.h"
#include "xz.h"

namespace micropixel::bouffalo {

bool DecodeEmbeddedXz(std::span<const uint8_t> input, std::span<uint8_t> output) {
    // The SDK's XZ port has a global scratch allocator. The font loader and
    // Guest workers use the same lock, and XZ_SINGLE uses output as its dictionary.
    static StaticSemaphore_t lock_storage;
    static const auto lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (!lock || xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) return false;
    constexpr uint32_t kScratchBytes = 32U * 1024U;
    auto* scratch = static_cast<uint8_t*>(kmalloc(kScratchBytes, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    if (!scratch) {
        (void)xSemaphoreGive(lock);
        return false;
    }
    simple_malloc_init(scratch, kScratchBytes);
    xz_crc32_init();
    xz_buf buffers{.in = input.data(),
                   .in_pos = 0,
                   .in_size = input.size(),
                   .out = output.data(),
                   .out_pos = 0,
                   .out_size = output.size()};
    auto* decoder = xz_dec_init(XZ_SINGLE, 0);
    const bool valid = decoder && xz_dec_run(decoder, &buffers) == XZ_STREAM_END && buffers.in_pos == input.size() &&
                       buffers.out_pos == output.size();
    if (decoder) xz_dec_end(decoder);
    simple_malloc_init(nullptr, 0);
    kfree(scratch);
    (void)xSemaphoreGive(lock);
    return valid;
}

}  // namespace micropixel::bouffalo
