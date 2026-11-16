// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <vector>

#include "device/device_registry.hpp"
#include "home_app.hpp"
#include "host/controller/app_hub.hpp"
#include "host/controller/app_manager.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "session.hpp"

namespace micropixel::pc {
class AppSessions final {
   public:
    AppSessions(std::vector<std::unique_ptr<PortableApp>>& apps, lv_display_t* display, device::DisplayInfo info,
                const platform::lvgl::SystemFontSet& fonts, SdlAudio& audio, device::Random& random,
                device::DeviceRegistry& registry, device::Battery& battery, bool multi, bool development_tasks = true);
    ~AppSessions();
    void Process();
    bool Activate(uint32_t app);
    bool EnsureWork(uint32_t app, bool explicit_user = false);
    bool ClearSchedules(uint32_t app);
    bool Remove(uint32_t app, bool uninstall);
    bool Removing(uint32_t app) const;
    int32_t RemovalResult(uint32_t app) const;
    void ShowHome();
    void Stop(uint32_t app, bool user = true);
    bool Install(uint32_t app, std::unique_ptr<PortableApp> package);
    Session* Find(uint32_t app);
    bool Pending(uint32_t app) const;
    PcInput& Input();
    MediaPlayer* Media(uint32_t app);
    const host::ManagerSnapshot& Snapshot() const;
    void Shutdown();
    bool Widget(uint32_t index, host::WidgetSnapshot& out);
    int32_t WidgetAction(const host::WidgetSnapshot&, uint16_t type, uint32_t value, uint64_t sequence,
                         uint32_t& ticket);
    micropixel_message_result_t WidgetResult(uint32_t ticket);
    struct AlarmState {
        bool ringing{};
        uint32_t count{};
        uint64_t revision{};
        std::array<char, 64> label{};
    };
    AlarmState Alarm();
    uint64_t NextHomeRequest();
    void StopAlarm();
    micropixel_wall_time_t Clock();
    struct PlannerSummary {
        uint64_t alarm_utc{}, note_utc{};
        uint32_t pending_notes{}, note_records{};
        std::array<char, 64> note{};
    };
    PlannerSummary Planner();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace micropixel::pc
