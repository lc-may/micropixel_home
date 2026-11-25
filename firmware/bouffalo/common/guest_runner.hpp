// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <span>
#include <string_view>

#include "freertos/task.h"
#include "runtime/session_runner.hpp"

namespace micropixel::platform::audio {
class AudioEngine;
}

namespace micropixel::bouffalo {
class WeatherState;
class MediaWidgets;

class GuestPanel;
class AlarmHost;

struct EmbeddedApp final {
    const char* name;
    std::span<const uint8_t> wasm;
    uint32_t unpacked_size;
    bool activity{};
    bool wireless_settings{};
    uint16_t component_flags{};
    bool can_set_time{};
    bool weather{};
    bool music{};
    uint16_t canvas_extent{};
    bool KeepService() const { return weather || music; }
};

struct GuestLaunchOptions final {
    uint64_t song{};
    bool alarm_test{};
    bool music_login{};
};

class GuestRunner final : public runtime::SessionRunner {
   public:
    GuestRunner(std::span<const EmbeddedApp> apps, platform::audio::AudioEngine& audio, GuestPanel& panel,
                AlarmHost& alarms, WeatherState& weather, uint32_t (*signal_writes)());

    [[nodiscard]] bool Initialize(MediaWidgets&);
    void Touch(int32_t x, int32_t y, bool pressed);
    [[nodiscard]] bool WantsText() const;
    [[nodiscard]] bool SubmitText(std::string_view text);
    [[nodiscard]] bool NavigateBack();
    bool BeginStart(uint32_t app, runtime::InstanceKey owner) override;
    void RequestStop() override;
    // These headless apps keep running behind the application directory.
    void RequestSuspend() override {}
    void RequestResume() override;
    void RequestBackgroundResume() override {}
    void SetVisible(bool visible) override;
    void SetLaunchOptions(GuestLaunchOptions options) { options_ = options; }
    [[nodiscard]] GuestPanel& Panel() { return panel_; }
    runtime::SessionObservation Observe() override;

   private:
    struct Session;
    enum class Phase { kStarting, kRunning, kExited };

    static void Worker(void* self);
    void Work();
    void ReapSession();

    std::span<const EmbeddedApp> apps_;
    platform::audio::AudioEngine& audio_;
    GuestPanel& panel_;
    AlarmHost& alarms_;
    WeatherState& weather_;
    MediaWidgets* media_widgets_{};
    GuestLaunchOptions options_;
    uint32_t (*signal_writes_)();
    TaskHandle_t worker_{};
    Session* session_{};
    std::atomic<Phase> phase_{Phase::kExited};
    uint32_t app_{};
    uint32_t touch_sequence_{};
    int32_t touch_x_{}, touch_y_{};
    bool touch_pressed_{};
    int32_t result_{};
};

}  // namespace micropixel::bouffalo
