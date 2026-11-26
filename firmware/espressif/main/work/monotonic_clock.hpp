// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "esp_timer.h"

namespace micropixel::work {

inline uint64_t MonotonicMicros() { return static_cast<uint64_t>(esp_timer_get_time()); }

}  // namespace micropixel::work
