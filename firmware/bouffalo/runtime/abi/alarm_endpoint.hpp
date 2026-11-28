// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/controller/alarm_scheduler.hpp"
#include "host/controller/system_clock.hpp"
#include "runtime/abi/service_registry.hpp"

namespace micropixel::platform::audio {
class AudioEngine;
}

namespace micropixel::bouffalo {

class AlarmHost final {
   public:
    bool Initialize(uint32_t clock_app);
    bool SetUtc(uint64_t utc, uint64_t now_ms, uint32_t fractional_ms = 0);
    micropixel_wall_time_t ReadClock(uint64_t now_ms);
    void Process(uint64_t now_ms, platform::audio::AudioEngine& audio);
    int32_t Call(uint32_t app, bool can_set_time, uint32_t method, const uint8_t* request, uint32_t length,
                 uint8_t* response, uint32_t capacity, uint32_t& size);

   private:
    uint64_t Utc(uint64_t now_ms) const;

    StaticSemaphore_t lock_storage_;
    SemaphoreHandle_t lock_{};
    host::SystemClock clock_;
    host::AlarmScheduler alarms_;
    uint32_t clock_app_{};
    uint64_t sync_utc_ms_{};
    uint64_t sync_ms_{};
    uint64_t next_tone_ms_{};
    bool ringing_{};
};

class AlarmEndpoint final : public runtime::ServiceHandler {
   public:
    AlarmEndpoint(AlarmHost& host, uint32_t app, bool can_set_time)
        : host_(host), app_(app), can_set_time_(can_set_time) {}
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;

   private:
    AlarmHost& host_;
    uint32_t app_;
    bool can_set_time_;
};

}  // namespace micropixel::bouffalo
