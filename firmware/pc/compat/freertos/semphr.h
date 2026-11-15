#pragma once
#include <condition_variable>

#include "freertos/FreeRTOS.h"
struct StaticSemaphore_t {
    std::mutex mutex;
    std::condition_variable ready;
    bool available{};
    bool allocated{};
};
using SemaphoreHandle_t = StaticSemaphore_t*;
SemaphoreHandle_t xSemaphoreCreateBinary();
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage);
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);
inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t*) { return xSemaphoreGive(sem); }
