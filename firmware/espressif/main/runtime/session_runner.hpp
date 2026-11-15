// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "runtime/instance.hpp"

namespace micropixel::runtime {
struct SessionObservation final {
    InstanceKey owner{};
    bool ready{};
    bool frame_ready{};
    bool suspended{};
    bool exited{};
    bool activity_available{true};
    bool background_eligible{true};
    int32_t result{};
};

// All methods are nonblocking. Observe also completes already-finished cleanup.
class SessionRunner {
   public:
    virtual ~SessionRunner() = default;
    virtual bool BeginStart(uint32_t app, InstanceKey owner) = 0;
    virtual void RequestStop() = 0;
    virtual void RequestSuspend() = 0;
    virtual void RequestResume() = 0;
    virtual void RequestBackgroundResume() { RequestResume(); }
    virtual void SetVisible(bool visible) = 0;
    [[nodiscard]] virtual SessionObservation Observe() = 0;
};
}  // namespace micropixel::runtime
