// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "bflb_mtimer.h"

namespace micropixel::work {

inline uint64_t MonotonicMicros() { return bflb_mtimer_get_time_us(); }

}  // namespace micropixel::work
