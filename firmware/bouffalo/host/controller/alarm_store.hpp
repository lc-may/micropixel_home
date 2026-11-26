// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/controller/alarm_scheduler.hpp"
#include "host/controller/system_clock.hpp"

namespace micropixel::bouffalo {

void RestoreClockAlarms(host::AlarmScheduler& alarms, host::SystemClock& clock, uint32_t app);
bool SaveClockAlarms(const host::AlarmScheduler& alarms, const host::SystemClock& clock, uint32_t app,
                     uint32_t updated_id = 0, const host::AlarmScheduler::Record* replacement = nullptr);

}  // namespace micropixel::bouffalo
