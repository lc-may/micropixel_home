// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bflb_mtimer.h"

static inline int64_t esp_timer_get_time(void) { return (int64_t)bflb_mtimer_get_time_us(); }
