// SPDX-License-Identifier: Apache-2.0
#pragma once

// Define DBG_TAG in each translation unit before including this header.
#include "esp_log.h"

#define MPX_LOGD(...) ESP_LOGD(DBG_TAG, __VA_ARGS__)
#define MPX_LOGI(...) ESP_LOGI(DBG_TAG, __VA_ARGS__)
#define MPX_LOGE(...) ESP_LOGE(DBG_TAG, __VA_ARGS__)
