// SPDX-License-Identifier: Apache-2.0
#pragma once

// Define DBG_TAG in each translation unit before including this header.
#include <cstdio>

#include "log.h"

#define MPX_LOGD(format, ...) LOG_D(format "\r\n" __VA_OPT__(, ) __VA_ARGS__)
#define MPX_LOGI(format, ...) LOG_I(format "\r\n" __VA_OPT__(, ) __VA_ARGS__)
#define MPX_LOGE(format, ...) LOG_E(format "\r\n" __VA_OPT__(, ) __VA_ARGS__)
