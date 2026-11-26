// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "mm.h"
#include "work/task_stack.h"

namespace micropixel::bouffalo {

// The controller and Guest interpreter keep their hot stacks in OCRAM.
// These I/O workers can use the unused part of the SDK's wireless heap.
class BackgroundTask final {
   public:
    bool Start(TaskFunction_t run, const char* name, uint32_t stack_bytes, void* argument, UBaseType_t priority) {
        if (handle_) return false;
        constexpr size_t kWirelessReserve = 64U * 1024U;
        const auto words = (stack_bytes + sizeof(StackType_t) - 1U) / sizeof(StackType_t);
        const auto bytes = words * sizeof(StackType_t);
        if (kfree_size(MM_HEAP_WRAM_0) >= bytes + kWirelessReserve) {
            stack_ = static_cast<StackType_t*>(kmalloc(bytes, MM_FLAG_HEAP_WRAM_0 | MM_FLAG_ALIGN_16BYTE));
            if (stack_) {
                handle_ = xTaskCreateStatic(run, name, words, argument, priority, stack_, &control_);
                if (handle_) return true;
                kfree(stack_);
                stack_ = nullptr;
            }
        }
        return mpx_create_task(run, name, stack_bytes, argument, priority, &handle_) == pdPASS;
    }

    [[nodiscard]] TaskHandle_t Handle() const { return handle_; }

    // Only used to unwind initialization before any work can be queued.
    void CancelStart() {
        if (handle_) vTaskDelete(handle_);
        handle_ = nullptr;
        kfree(stack_);
        stack_ = nullptr;
    }

   private:
    StaticTask_t control_{};
    TaskHandle_t handle_{};
    StackType_t* stack_{};
};

}  // namespace micropixel::bouffalo
