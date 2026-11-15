#pragma once
#include <condition_variable>

#include "freertos/FreeRTOS.h"
struct StaticQueue_t {
    std::mutex mutex;
    std::condition_variable ready, space;
    uint8_t* bytes{};
    size_t capacity{}, item_size{}, head{}, count{};
    bool allocated{};
};
using QueueHandle_t = StaticQueue_t*;
QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t item_size);
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size, uint8_t* bytes, StaticQueue_t* storage);
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t timeout);
BaseType_t xQueueSendToFront(QueueHandle_t q, const void* item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t timeout);
BaseType_t xQueuePeek(QueueHandle_t q, void* item, TickType_t timeout);
BaseType_t xQueueReset(QueueHandle_t q);
BaseType_t xQueueOverwrite(QueueHandle_t q, const void* item);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);
