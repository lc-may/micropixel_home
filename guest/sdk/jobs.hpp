// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdk/result.hpp"
namespace micropixel {
struct JobPlan {
    uint32_t id{1}, interval_seconds{900}, deadline_ms{20000};
    uint64_t next_utc{};
};
struct WallTime {
    uint64_t utc{};
    int32_t offset_minutes{};
    bool valid{};
};
class Jobs final {
   public:
    [[nodiscard]] Result<void> Register(const JobPlan&) const;
    [[nodiscard]] Result<void> Cancel(uint32_t id) const;
    [[nodiscard]] Result<void> Enter(uint32_t ticket) const;
    [[nodiscard]] Result<void> Complete(uint32_t ticket) const;
    [[nodiscard]] Result<WallTime> Now() const;
};
}  // namespace micropixel
