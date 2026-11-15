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
                device::DeviceRegistry& registry, device::Battery& battery, bool multi);
    ~AppSessions();
    void Process();
    bool Activate(uint32_t app);
    void ShowHome();
    void Stop(uint32_t app);
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

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace micropixel::pc
