// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdio.h>

#define ESP_LOGI(tag, format, ...) printf("I [%s] " format "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGW(tag, format, ...) printf("W [%s] " format "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGE(tag, format, ...) printf("E [%s] " format "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define ESP_LOGD(tag, format, ...)                               \
    do {                                                         \
        if (0) ESP_LOGI(tag, format __VA_OPT__(, ) __VA_ARGS__); \
    } while (0)
