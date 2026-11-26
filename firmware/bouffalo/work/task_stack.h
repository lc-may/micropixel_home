// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif
static inline BaseType_t mpx_create_task(TaskFunction_t function, const char *name, uint32_t stack_bytes,
                                         void *argument, UBaseType_t priority, TaskHandle_t *task) {
    const size_t stack_words = stack_bytes / sizeof(StackType_t) + (stack_bytes % sizeof(StackType_t) != 0U);
    const configSTACK_DEPTH_TYPE maximum_depth = (configSTACK_DEPTH_TYPE)-1;
    if (stack_words == 0 || stack_words > maximum_depth) {
        if (task) *task = NULL;
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    return xTaskCreate(function, name, (configSTACK_DEPTH_TYPE)stack_words, argument, priority, task);
}
static inline size_t mpx_stack_free_bytes(TaskHandle_t task) {
    return (size_t)uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
}
#ifdef __cplusplus
}
#endif
