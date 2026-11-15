#pragma once
#include <cstdint>
#include <mutex>

#include "sdkconfig.h"
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr unsigned configMAX_PRIORITIES = 25;
// Accept the firmware's two affinity identifiers; Linux schedules the workers.
constexpr unsigned configNUMBER_OF_CORES = 2;
constexpr unsigned configTICK_RATE_HZ = 1000;
constexpr BaseType_t pdTRUE = 1, pdFALSE = 0, pdPASS = 1, pdFAIL = 0;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr unsigned portTICK_PERIOD_MS = 1;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED \
    {                                \
    }
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
#define taskENTER_CRITICAL(p) portENTER_CRITICAL(p)
#define taskEXIT_CRITICAL(p) portEXIT_CRITICAL(p)
#define portENTER_CRITICAL_ISR(p) portENTER_CRITICAL(p)
#define portEXIT_CRITICAL_ISR(p) portEXIT_CRITICAL(p)
#define portYIELD_FROM_ISR() ((void)0)
#define configASSERT(condition)             \
    do {                                    \
        if (!(condition)) std::terminate(); \
    } while (0)
