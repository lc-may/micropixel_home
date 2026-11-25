// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

#include "runtime/abi/service_registry.hpp"
#include "runtime/event_queue.hpp"

namespace micropixel::bouffalo {

// A Guest owns its timers. The same task dispatches ABI calls and services
// deadlines at event_wait, without a timer callback retaining a session pointer.
class TimerEndpoint final : public runtime::ServiceHandler {
   public:
    explicit TimerEndpoint(int64_t origin_us) : origin_us_(origin_us) {}
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& response_size) override;
    runtime::EventWaitResult Wait(runtime::EventQueue& queue, micropixel_event_t& event, uint64_t timeout_us);

   private:
    struct Timer {
        uint64_t deadline_us{};
        uint64_t period_us{};
        uint64_t last_us{};
        uint32_t handle{};
        uint32_t generation{};
        uint32_t sequence{};
    };

    uint64_t Now() const;
    Timer* Find(uint32_t handle);
    int32_t Create(uint8_t* response, uint32_t capacity, uint32_t& response_size);
    void Expire(Timer& timer, uint64_t now, micropixel_event_t& event);

    int64_t origin_us_;
    std::array<Timer, runtime::limits::kMaxTimers> timers_{};
};

}  // namespace micropixel::bouffalo
