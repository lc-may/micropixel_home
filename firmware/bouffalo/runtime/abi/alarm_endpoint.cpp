// SPDX-License-Identifier: Apache-2.0
#include "alarm_endpoint.hpp"

#include <cstdio>
#include <cstring>

#include "bflb_mtimer.h"
#include "platform/audio/audio_engine.hpp"

#if defined(CONFIG_LITTLEFS)
#include "host/controller/alarm_store.hpp"
#endif

namespace micropixel::bouffalo {
namespace {

constexpr uint64_t kMinUtc = 1577836800;
constexpr uint64_t kMaxUtc = 4102444800;

struct Lock final {
    explicit Lock(SemaphoreHandle_t mutex) : lock(mutex) { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(lock); }
    SemaphoreHandle_t lock;
};

template <typename T>
int32_t Reply(const T& value, uint8_t* response, uint32_t capacity, uint32_t& size) {
    size = sizeof(value);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(response, &value, size);
    return MICROPIXEL_STATUS_OK;
}

}  // namespace

bool AlarmHost::Initialize(uint32_t clock_app) {
    lock_ = xSemaphoreCreateMutexStatic(&lock_storage_);
    if (!lock_) return false;
    clock_app_ = clock_app;
#if defined(CONFIG_LITTLEFS)
    RestoreClockAlarms(alarms_, clock_, clock_app_);
    alarms_.SetPersistence(
        {this, [](void* context, const host::AlarmScheduler::Record* replacement, uint32_t app, uint32_t id) {
             const auto& self = *static_cast<AlarmHost*>(context);
             return app != self.clock_app_ || SaveClockAlarms(self.alarms_, self.clock_, app, id, replacement);
         }});
#endif
    return true;
}

bool AlarmHost::SetUtc(uint64_t utc, uint64_t now_ms, uint32_t fractional_ms) {
    if (utc < kMinUtc || utc > kMaxUtc || fractional_ms >= 1000U) return false;
    Lock lock(lock_);
    sync_utc_ms_ = utc * 1000U + fractional_ms;
    sync_ms_ = now_ms;
    return true;
}

uint64_t AlarmHost::Utc(uint64_t now_ms) const {
    if (sync_utc_ms_ == 0 || now_ms <= sync_ms_) return sync_utc_ms_ / 1000U;
    return (sync_utc_ms_ + (now_ms - sync_ms_)) / 1000U;
}

micropixel_wall_time_t AlarmHost::ReadClock(uint64_t now_ms) {
    Lock lock(lock_);
    return clock_.Now(Utc(now_ms));
}

void AlarmHost::Process(uint64_t now_ms, platform::audio::AudioEngine& audio) {
    bool play = false;
    {
        Lock lock(lock_);
        const auto now = clock_.Now(Utc(now_ms));
        alarms_.Process(now_ms, now.utc, now.offset_minutes, now.valid);
        if (alarms_.Ringing() != ringing_) {
            ringing_ = alarms_.Ringing();
            puts(ringing_ ? "MicroPixel alarm ringing" : "MicroPixel alarm cleared");
        }
        play = ringing_ && now_ms >= next_tone_ms_;
        if (play) next_tone_ms_ = now_ms + 700;
    }
    if (play) {
        const micropixel_audio_tone_t tone{.size = sizeof(tone),
                                           .waveform = MICROPIXEL_AUDIO_WAVE_SINE,
                                           .volume_per_mille = 650,
                                           .frequency_millihz = 1000000,
                                           .duration_ms = 340,
                                           .attack_ms = 15,
                                           .release_ms = 30};
        (void)audio.ResumeAll();
        (void)audio.PlayTone(tone);
    }
}

int32_t AlarmHost::Call(uint32_t app, bool can_set_time, uint32_t method, const uint8_t* request, uint32_t length,
                        uint8_t* response, uint32_t capacity, uint32_t& size) {
    Lock lock(lock_);
    const uint64_t system_utc = Utc(bflb_mtimer_get_time_us() / 1000);
    const auto now = clock_.Now(system_utc);
    switch (method) {
        case MICROPIXEL_ALARM_NOW:
            if (length != 0) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return Reply(now, response, capacity, size);
        case MICROPIXEL_ALARM_REGISTER: {
            micropixel_alarm_plan_t plan{};
            if (length != sizeof(plan)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            std::memcpy(&plan, request, sizeof(plan));
            return alarms_.Register(app, plan, now.utc, now.valid);
        }
        case MICROPIXEL_ALARM_STOP:
            if (length != 0) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            alarms_.StopRinging(app);
            return MICROPIXEL_STATUS_OK;
        case MICROPIXEL_ALARM_CONFIGURE_TIME: {
            if (!can_set_time) return MICROPIXEL_STATUS_PERMISSION_DENIED;
            micropixel_wall_time_t time{};
            if (length != sizeof(time)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            std::memcpy(&time, request, sizeof(time));
            if (time.size != sizeof(time) || time.reserved0 || time.reserved1 || time.valid > 1 ||
                (time.utc && (time.utc < kMinUtc || time.utc > kMaxUtc || system_utc == 0)))
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            const host::SystemClock::Settings settings{
                time.offset_minutes, time.valid != 0,
                time.utc ? static_cast<int64_t>(time.utc) - static_cast<int64_t>(system_utc) : 0};
            auto clock = clock_;
            if (!clock.Restore(settings)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
#if defined(CONFIG_LITTLEFS)
            if (app == clock_app_ && !SaveClockAlarms(alarms_, clock, app)) return MICROPIXEL_STATUS_INTERNAL;
#endif
            clock_ = clock;
            return MICROPIXEL_STATUS_OK;
        }
        case MICROPIXEL_ALARM_GET:
        case MICROPIXEL_ALARM_REMOVE: {
            micropixel_handle_request_t handle{};
            if (length != sizeof(handle)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            std::memcpy(&handle, request, sizeof(handle));
            if (handle.size != sizeof(handle) || handle.reserved0 || handle.handle < 1 || handle.handle > 8)
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            if (method == MICROPIXEL_ALARM_REMOVE) return alarms_.Remove(app, handle.handle);
            const auto* record = alarms_.Get(app, handle.handle);
            return record ? Reply(record->plan, response, capacity, size) : MICROPIXEL_STATUS_NOT_FOUND;
        }
        default:
            return MICROPIXEL_STATUS_UNSUPPORTED;
    }
}

runtime::ServiceDescriptor AlarmEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_ALARM,
            .interface_major = 1,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .capabilities = 1U,
            .max_request_bytes = sizeof(micropixel_alarm_plan_t),
            .max_response_bytes = sizeof(micropixel_alarm_plan_t)};
}

int32_t AlarmEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                            uint32_t capacity, uint32_t& size) {
    return host_.Call(app_, can_set_time_, method, request, length, response, capacity, size);
}

}  // namespace micropixel::bouffalo
