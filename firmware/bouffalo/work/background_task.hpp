// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "work/task_stack.h"

namespace micropixel::bouffalo {

class BackgroundTask final {
   public:
    bool Start(TaskFunction_t run, const char* name, uint32_t stack_bytes, void* argument, UBaseType_t priority) {
        if (handle_) return false;
        return mpx_create_task(run, name, stack_bytes, argument, priority, &handle_) == pdPASS;
    }

    [[nodiscard]] TaskHandle_t Handle() const { return handle_; }

    // Only used to unwind initialization before any work can be queued.
    void CancelStart() {
        if (handle_) vTaskDelete(handle_);
        handle_ = nullptr;
    }

   private:
    TaskHandle_t handle_{};
};

}  // namespace micropixel::bouffalo
