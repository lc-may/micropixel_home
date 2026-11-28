// SPDX-License-Identifier: Apache-2.0
#include "guest_runner.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>

#include "bflb_mtimer.h"
#include "bflb_sec_trng.h"
#include "host/controller/media_widgets.hpp"
#include "host/network/network.hpp"
#include "mm.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/audio/media.hpp"
#include "runtime/abi/alarm_endpoint.hpp"
#include "runtime/abi/app_storage_endpoint.hpp"
#include "runtime/abi/audio_endpoint.hpp"
#include "runtime/abi/bluetooth_endpoint.hpp"
#include "runtime/abi/network_endpoint.hpp"
#include "runtime/abi/resource_endpoint.hpp"
#include "runtime/abi/timer_endpoint.hpp"
#include "runtime/abi/weather_endpoint.hpp"
#include "runtime/abi/wireless_endpoint.hpp"
#include "runtime/components.hpp"
#if WASM_ENABLE_AOT
#include "runtime/bundle/app_store.hpp"
#endif
#include "runtime/graphics/guest_panel.hpp"
#include "runtime/wamr/guest_runtime.hpp"
#include "work/task_policy.hpp"
#include "work/task_stack.h"

namespace micropixel::bouffalo {
namespace {

using runtime::EventQueue;
using runtime::InstanceKey;
using runtime::ServiceRegistry;
using runtime::SessionObservation;

constexpr uint32_t kGuestTaskStackBytes = 12U * 1024U;

template <typename T>
int32_t Reply(const T& value, uint8_t* response, uint32_t capacity, uint32_t& size) {
    size = sizeof(value);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(response, &value, size);
    return MICROPIXEL_STATUS_OK;
}

class SystemEndpoint final : public runtime::ServiceHandler {
   public:
    explicit SystemEndpoint(GuestLaunchOptions options) : options_(options) {}
    runtime::ServiceDescriptor Describe() const override {
        return {
            .service_id = MICROPIXEL_SERVICE_SYSTEM,
            .interface_major = MICROPIXEL_SYSTEM_INTERFACE_MAJOR,
            .interface_minor = MICROPIXEL_SYSTEM_INTERFACE_MINOR,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_response_bytes = sizeof(micropixel_system_launch_arguments_response_t),
        };
    }

    int32_t Call(uint32_t method, const uint8_t*, uint32_t, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override {
        static constexpr micropixel_system_locale_response_t kLocale{
            .size = sizeof(kLocale),
            .tag_length = 2,
            .tag = "en",
        };
        static constexpr micropixel_system_launch_arguments_response_t kArguments{.size = sizeof(kArguments)};
        if (method == MICROPIXEL_SYSTEM_METHOD_GET_LOCALE) return Reply(kLocale, response, capacity, size);
        if (method == MICROPIXEL_SYSTEM_METHOD_GET_LAUNCH_ARGUMENTS) {
            if (options_.song) {
                micropixel_system_launch_arguments_response_t song{.size = sizeof(song), .count = 1};
                constexpr std::string_view kSong = "--song=";
                std::memcpy(song.bytes, kSong.data(), kSong.size());
                const auto end =
                    std::to_chars(song.bytes + kSong.size(), song.bytes + sizeof(song.bytes) - 1, options_.song);
                if (end.ec != std::errc{}) return MICROPIXEL_STATUS_INTERNAL;
                *end.ptr = '\0';
                song.bytes_length = end.ptr - song.bytes + 1;
                return Reply(song, response, capacity, size);
            }
            if (options_.music_login) {
                static constexpr micropixel_system_launch_arguments_response_t kLogin{
                    .size = sizeof(kLogin), .count = 1, .bytes_length = sizeof("--login"), .bytes = "--login"};
                return Reply(kLogin, response, capacity, size);
            }
            if (!options_.alarm_test) return Reply(kArguments, response, capacity, size);
            static constexpr micropixel_system_launch_arguments_response_t kAlarm{
                .size = sizeof(kAlarm), .count = 1, .bytes_length = sizeof("--alarm-test"), .bytes = "--alarm-test"};
            return Reply(kAlarm, response, capacity, size);
        }
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

   private:
    GuestLaunchOptions options_;
};

class RandomEndpoint final : public runtime::ServiceHandler {
   public:
    runtime::ServiceDescriptor Describe() const override {
        return {
            .service_id = MICROPIXEL_SERVICE_RANDOM,
            .interface_major = MICROPIXEL_RANDOM_INTERFACE_MAJOR,
            .interface_minor = MICROPIXEL_RANDOM_INTERFACE_MINOR,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_response_bytes = sizeof(micropixel_random_u32_response_t),
        };
    }

    int32_t Call(uint32_t method, const uint8_t*, uint32_t, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override {
        if (method != MICROPIXEL_RANDOM_METHOD_GET_U32) return MICROPIXEL_STATUS_UNSUPPORTED;
        micropixel_random_u32_response_t value{.size = sizeof(value)};
        if (bflb_trng_readlen(reinterpret_cast<uint8_t*>(&value.value), sizeof(value.value)) != 0) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
        return Reply(value, response, capacity, size);
    }
};

class LifecycleEndpoint final : public runtime::ServiceHandler {
   public:
    explicit LifecycleEndpoint(runtime::Components& owner) : owner_(owner) {}
    runtime::ServiceDescriptor Describe() const override {
        return {.service_id = MICROPIXEL_SERVICE_LIFECYCLE,
                .interface_major = 1,
                .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
                .capabilities = owner_.Available() ? 1U : 0U,
                .max_request_bytes = sizeof(micropixel_component_ack_t),
                .max_response_bytes = sizeof(micropixel_component_info_t)};
    }
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override {
        return owner_.Call(method, request, length, response, capacity, size);
    }

   private:
    runtime::Components& owner_;
};

}  // namespace

struct GuestRunner::Session final {
    Session(InstanceKey key, platform::audio::AudioEngine& engine, GuestPanel& panel, AlarmHost& alarm_host,
            WeatherState& weather, MediaWidgets* media, const EmbeddedApp& app, uint32_t app_id,
            GuestLaunchOptions options)
        : owner(key),
          system(options),
          audio(engine),
#if defined(CONFIG_FHOST)
          output(engine, pcm, owner.slot + 1, app.music ? MediaBackend() : nullptr),
#else
          output(engine, pcm, owner.slot + 1, nullptr),
#endif
          touch(events, origin_us, panel),
          resources(panel.Bitmaps(), memory),
          alarm(alarm_host, app_id, app.clock),
          media_widgets(app.music ? media : nullptr, {app_id, key, 0}),
          widget(app.weather ? &weather : nullptr, media_widgets),
          messages(media_widgets),
          storage(app.bundle_id),
          components(events, services, app.component_flags,
                     {.context = this,
                      .now =
                          [](void* raw) {
                              auto& session = *static_cast<Session*>(raw);
                              return static_cast<uint64_t>(bflb_mtimer_get_time_us() - session.origin_us);
                          },
                      .stop = [](void* raw) { return static_cast<Session*>(raw)->Stop(); },
                      .scope =
                          [](void* raw, uint32_t token, const micropixel_component_info_t& info) {
                              auto& session = *static_cast<Session*>(raw);
                              session.audio.SetComponentScope(token);
                              session.media_widgets.SetScope(token, info);
#if defined(CONFIG_FHOST)
                              if (auto* http = NetworkBackend(session.owner.slot)) http->SetComponentScope(token);
#endif
                          },
                      .changed =
                          [](void* raw, const micropixel_component_info_t& info) {
                              static_cast<Session*>(raw)->media_widgets.SetComponents(info);
                          },
                      .stop_audio = [](void* raw,
                                       uint32_t token) { static_cast<Session*>(raw)->audio.StopComponent(token); }}),
#if defined(CONFIG_FHOST)
          network(NetworkBackend(owner.slot)),
#endif
          services(system, random, timers, output, panel, touch, wireless,
#if defined(CONFIG_BT_A2DP)
                   bluetooth,
#endif
                   resources, lifecycle, alarm,
#if defined(CONFIG_FHOST)
                   network,
#endif
                   widget, messages, storage) {
        wireless.Authorize(app.wireless_settings);
#if defined(CONFIG_BT_A2DP)
        bluetooth.Authorize(app.wireless_settings, app.bluetooth_speaker);
#endif
        if (app.component_flags) {
            if (!app.weather && !app.bluetooth_speaker) components.SetVisible(app.activity);
            guest.components = &components;
        }
    }

    bool Stop() {
        guest.stopping.store(true, std::memory_order_release);
        media_widgets.Quiesce();
        const micropixel_event_t event{.size = sizeof(event),
                                       .event_id = MICROPIXEL_CORE_EVENT_STOP,
                                       .timestamp_us = static_cast<uint64_t>(bflb_mtimer_get_time_us() - origin_us),
                                       .sequence = 1U};
        return events.RequestStop(event);
    }

#if WASM_ENABLE_AOT
    PackedApp packed_app;
#endif
    const InstanceKey owner;
    const int64_t origin_us{static_cast<int64_t>(bflb_mtimer_get_time_us())};
    EventQueue events;
    SystemEndpoint system;
    RandomEndpoint random;
    TimerEndpoint timers{origin_us};
    device::AudioService audio;
    runtime::PcmStreamService pcm{audio, events, origin_us};
    AudioEndpoint output;
    TouchEndpoint touch;
    WirelessEndpoint wireless;
#if defined(CONFIG_BT_A2DP)
    BluetoothEndpoint bluetooth;
#endif
    GuestMemory memory;
    ResourceEndpoint resources;
    AlarmEndpoint alarm;
    MediaWidgetSession media_widgets;
    HomeWidgetEndpoint widget;
    MediaMessageEndpoint messages;
    AppStorageEndpoint storage;
    runtime::Components components;
    LifecycleEndpoint lifecycle{components};
#if defined(CONFIG_FHOST)
    runtime::NetworkServiceEndpoint network;
#endif
    ServiceRegistry services;
    GuestContext guest{events, services, origin_us, &timers, nullptr, &memory};
};

GuestRunner::GuestRunner(std::span<const EmbeddedApp> apps, platform::audio::AudioEngine& audio, GuestPanel& panel,
                         AlarmHost& alarms, WeatherState& weather, uint32_t (*signal_writes)())
    : apps_(apps), audio_(audio), panel_(panel), alarms_(alarms), weather_(weather), signal_writes_(signal_writes) {}

bool GuestRunner::Initialize(MediaWidgets& media_widgets) {
    media_widgets_ = &media_widgets;
    return mpx_create_task(Worker, "mpx_guest", kGuestTaskStackBytes, this, task_policy::kGuestPriority, &worker_) ==
           pdPASS;
}

bool GuestRunner::BeginStart(uint32_t app, InstanceKey owner) {
    if (worker_ == nullptr || session_ != nullptr || app >= apps_.size() || !apps_[app].Available()) return false;
    auto* memory = static_cast<Session*>(kmalloc(sizeof(Session), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    if (memory == nullptr) return false;
    if (!panel_.Initialize(apps_[app].canvas_extent)) {
        kfree(memory);
        return false;
    }
    touch_pressed_ = false;
    session_ =
        std::construct_at(memory, owner, audio_, panel_, alarms_, weather_, media_widgets_, apps_[app], app, options_);
    options_ = {};
#if WASM_ENABLE_AOT
    if (apps_[app].bundle_id) {
        session_->packed_app = OrionAppStore::Read(apps_[app].bundle_id);
        if (!session_->packed_app) {
            ReapSession();
            return false;
        }
    }
#endif
    if (!session_->events.valid() || !session_->pcm.valid()) {
        ReapSession();
        return false;
    }
    session_->media_widgets.Bind(session_->events);
    app_ = app;
    phase_.store(Phase::kStarting, std::memory_order_release);
    xTaskNotifyGive(worker_);
    return true;
}

void GuestRunner::Touch(int32_t x, int32_t y, bool pressed) {
    if (!session_ || phase_.load(std::memory_order_acquire) != Phase::kRunning) return;
    if (!pressed && !touch_pressed_) return;
    panel_.MapTouch(x, y);
    if (pressed && touch_pressed_ && x == touch_x_ && y == touch_y_) return;
    const uint16_t phase =
        pressed ? (touch_pressed_ ? MICROPIXEL_TOUCH_MOVE : MICROPIXEL_TOUCH_DOWN) : MICROPIXEL_TOUCH_UP;
    micropixel_event_t event{.size = sizeof(event),
                             .event_id = MICROPIXEL_INPUT_EVENT_TOUCH,
                             .service_id = MICROPIXEL_SERVICE_INPUT,
                             .timestamp_us = static_cast<uint64_t>(bflb_mtimer_get_time_us() - session_->origin_us),
                             .sequence = ++touch_sequence_};
    const micropixel_touch_event_payload_t touch{.x = x, .y = y, .phase = phase};
    std::memcpy(event.payload, &touch, sizeof(touch));
    const bool accepted = phase == MICROPIXEL_TOUCH_MOVE
                              ? session_->events.PushTouchMove(event) != runtime::TouchPushResult::kFailed
                              : session_->events.PushAdvisory(event);
    if (accepted) {
        touch_pressed_ = pressed;
        touch_x_ = x;
        touch_y_ = y;
    }
}

bool GuestRunner::NavigateBack() {
    if (!session_ || phase_.load(std::memory_order_acquire) != Phase::kRunning) return false;
    const micropixel_event_t event{
        .size = sizeof(event),
        .event_id = MICROPIXEL_INPUT_EVENT_NAVIGATE_BACK,
        .service_id = MICROPIXEL_SERVICE_INPUT,
        .timestamp_us = static_cast<uint64_t>(bflb_mtimer_get_time_us() - session_->origin_us),
        .sequence = ++touch_sequence_};
    return session_->events.PushAdvisory(event);
}

bool GuestRunner::WantsText() const {
    return session_ && phase_.load(std::memory_order_acquire) == Phase::kRunning && session_->touch.WantsText();
}

bool GuestRunner::SubmitText(std::string_view text) {
    return session_ && phase_.load(std::memory_order_acquire) == Phase::kRunning && session_->touch.SubmitText(text);
}

void GuestRunner::RequestStop() {
    if (session_ == nullptr || phase_.load(std::memory_order_acquire) == Phase::kExited) return;
    (void)session_->Stop();
}

void GuestRunner::SetVisible(bool visible) {
    if (session_ && apps_[app_].component_flags) session_->components.SetVisible(visible);
    panel_.SetVisible(visible);
    if (!visible) {
        touch_pressed_ = false;
        if (session_) session_->touch.ClearText();
    }
}

void GuestRunner::RequestResume() {
    if (session_ && apps_[app_].KeepService()) session_->components.EnsureActivity();
}

SessionObservation GuestRunner::Observe() {
    if (session_ == nullptr) return {};
    const auto phase = phase_.load(std::memory_order_acquire);
    SessionObservation observation{.owner = session_->owner,
                                   .frame_ready = apps_[app_].activity && panel_.HasFrame(),
                                   .activity_available = apps_[app_].activity};
    if (phase == Phase::kRunning && apps_[app_].bluetooth_speaker) {
        observation.background_eligible = session_->components.Info().service_state != 0;
    }
    if (phase == Phase::kRunning && apps_[app_].KeepService() && !session_->components.Info().activity_state) {
        observation.activity_available = false;
        if (observation.frame_ready) panel_.Reset();
        observation.frame_ready = false;
    }
    if (phase == Phase::kExited) {
        observation.exited = true;
        observation.result = result_;
        ReapSession();
    }
    observation.ready = phase == Phase::kRunning;
    return observation;
}

void GuestRunner::ReapSession() {
    session_->media_widgets.Unbind();
    std::destroy_at(session_);
    kfree(session_);
    session_ = nullptr;
    panel_.Release();
}

void GuestRunner::Worker(void* self) { static_cast<GuestRunner*>(self)->Work(); }

void GuestRunner::Work() {
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Acquire the session and app published by the controller.
        (void)phase_.load(std::memory_order_acquire);
        auto& session = *session_;
        const auto& app = apps_[app_];
        const uint32_t previous_writes = signal_writes_();
        phase_.store(Phase::kRunning, std::memory_order_release);
        std::printf("MicroPixel app %s started\n", app.name);
#if WASM_ENABLE_AOT
        const auto started = bflb_mtimer_get_time_us();
        LoadedApp bundle;
        result_ = -1;
        if (bundle.Open(session.packed_app)) {
            const auto size = bundle.Bundle().payload_size;
            auto image = bundle.TakeImage();
            session.resources.BindBundle(bundle.Bundle());
            result_ = RunGuestImage(session.guest, {image.release(), size}, app.name, app.linear_growth_pages, started);
        } else {
            std::puts("MicroPixel Guest Bundle could not be opened");
        }
#else
        result_ = RunGuest(session.guest, app.wasm, app.unpacked_size, app.name, app.linear_growth_pages);
#endif
        (void)session.output.Stop();
        std::printf("MicroPixel app %s exit=%ld, nonzero DMA writes=%lu, stack free=%lu bytes\n", app.name,
                    static_cast<long>(result_), static_cast<unsigned long>(signal_writes_() - previous_writes),
                    static_cast<unsigned long>(mpx_stack_free_bytes(nullptr)));
        phase_.store(Phase::kExited, std::memory_order_release);
    }
}

}  // namespace micropixel::bouffalo
