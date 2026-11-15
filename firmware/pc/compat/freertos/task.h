#pragma once
#include <atomic>
#include <condition_variable>
#include <thread>

#include "freertos/FreeRTOS.h"
struct PcTask {
    std::thread worker;
    std::mutex mutex;
    std::condition_variable ready;
    uint32_t notifications{};
    std::atomic<bool> finished{true};
};
using TaskHandle_t = PcTask*;
using TaskFunction_t = void (*)(void*);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char*, uint32_t, void* ctx, UBaseType_t, TaskHandle_t* out,
                                   BaseType_t);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* ctx, UBaseType_t priority,
                       TaskHandle_t* out);
TaskHandle_t xTaskGetCurrentTaskHandle();
void vTaskDelete(TaskHandle_t task);
inline void vTaskSuspend(TaskHandle_t) {}  // Used only at shared workers' exit.
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount();
void xTaskNotifyGive(TaskHandle_t task);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout);
inline unsigned uxTaskGetStackHighWaterMark(void*) { return 0; }
inline unsigned uxTaskGetStackHighWaterMark2(void*) { return 0; }
namespace micropixel::pc {
void JoinTasks();
}
