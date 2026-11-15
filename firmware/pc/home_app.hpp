// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <memory>
#include <vector>

#include "host/controller/app_hub.hpp"
#include "lvgl.h"
#include "media.hpp"
#include "portable_app.hpp"

namespace micropixel::pc {
class AppSessions;
// Host system application. All methods run on the UI thread under the LVGL lock.
class HomeApp final {
   public:
    HomeApp(std::vector<std::unique_ptr<PortableApp>>& apps, MediaPlayer& media, SdlAudio& audio,
            AppSessions* sessions = nullptr);
    ~HomeApp();
    void Show(int width, int height);
    void Hide();
    void Update();
    void SetRunning(const PortableApp* app);
    void SetMedia(MediaPlayer& media) { media_ = &media; }
    bool Visible() const { return root_ != nullptr; }
    int requested{-1};
    int stop_requested{-1};
    bool market_requested{}, music_requested{}, resume_requested{}, close_requested{};

   private:
    static void Launch(lv_event_t*);
    static void Action(lv_event_t*);
    static void WidgetEvent(lv_event_t*);
    void UpdateWidgets();
    void SendWidget(const host::WidgetSnapshot&, uint16_t type, uint32_t value);
    std::vector<std::unique_ptr<PortableApp>>& apps_;
    MediaPlayer* media_;
    SdlAudio& audio_;
    AppSessions* sessions_{};
    const PortableApp* running_{};
    lv_obj_t *root_{}, *catalog_{}, *running_title_{}, *status_{}, *resume_{}, *pause_{}, *close_{};
    int32_t scroll_y_{};
    int64_t updated_at_{};
    int width_{}, height_{}, catalog_top_{};
    lv_obj_t *widget_strip_{}, *next_{};
    std::array<host::WidgetSnapshot, 8> widgets_{};
    std::array<bool, 8> widget_valid_{};
    std::array<uint32_t, 8> logged_revision_{};
    std::array<bool, 8> logged_online_{};
    std::array<lv_obj_t*, 8> widget_cards_{}, widget_titles_{}, widget_subtitles_{}, widget_refresh_{};
    uint32_t media_widget_{UINT32_MAX}, widget_ticket_{}, pending_value_{};
    uint16_t pending_type_{};
};
}  // namespace micropixel::pc
