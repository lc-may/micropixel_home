// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdk/fixed_string.hpp"
#include "sdk/jobs.hpp"
namespace micropixel {
struct AlarmPlan {
    uint32_t id{1};
    bool enabled{true}, weekly{};
    uint64_t once_utc{};
    uint16_t minute_of_day{}, weekdays{};  // Monday bit 0 ... Sunday bit 6.
    FixedString<64> label;
};
class Alarms final {
   public:
    [[nodiscard]] Result<WallTime> Now() const;
    [[nodiscard]] Result<void> Register(const AlarmPlan&) const;
    [[nodiscard]] Result<void> Remove(uint32_t id) const;
    [[nodiscard]] Result<AlarmPlan> Get(uint32_t id) const;
    [[nodiscard]] Result<void> StopRinging() const;
    // Restricted to the trusted Clock Activity. utc=0 uses the system clock.
    [[nodiscard]] Result<void> ConfigureTime(bool valid, int32_t offset_minutes, uint64_t utc = 0) const;
};
}  // namespace micropixel
