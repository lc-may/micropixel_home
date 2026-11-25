// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "host/controller/app_hub.hpp"
#include "host/controller/app_manager.hpp"
#include "lvgl.h"
#include "orion_touch.hpp"

namespace micropixel::bouffalo {

class WeatherState;
class MediaWidgets;

enum class HomeAppRole : uint8_t { kSettings, kClock, kWeather, kMusic, kNews, kDiagnostic };

class OrionHome final {
   public:
    struct Callbacks final {
        void* context;
        void (*launch)(void*, uint32_t);
        void (*home)(void*);
        void (*back)(void*);
        void (*touch)(void*, int32_t, int32_t, bool);
        bool (*submit_text)(void*, const char*);
        void (*volume)(void*, uint8_t);
        void (*media_action)(void*, const host::WidgetSnapshot&, uint16_t, uint32_t);
    };

    [[nodiscard]] bool Initialize(Callbacks callbacks);
    void AddApp(uint32_t index, const char* title, HomeAppRole role);
    void Update(const micropixel_wall_time_t& wall, uint64_t next_alarm);
    void UpdateWeather(const WeatherState& state);
    void UpdateMusic(MediaWidgets& widgets);
    void LaunchStatus(const char* text);
    [[nodiscard]] bool CaptureFrame() const;
    [[nodiscard]] uint32_t Frames() const;
    lv_obj_t* GuestSurface(bool visible);
    void ShowKeyboard(bool visible);
    void Refresh();

   private:
    struct AppTile final {
        OrionHome* owner{};
        uint32_t app{};
    };

    static void Launch(lv_event_t* event);
    static void Back(lv_event_t* event);
    static void ToggleCatalog(lv_event_t* event);
    static void ChangeVolume(lv_event_t* event);
    static void TogglePlayback(lv_event_t* event);
    static void NextTrack(lv_event_t* event);
    static void KeyboardAction(lv_event_t* event);
    static void ReadTouch(lv_indev_t* pointer, lv_indev_data_t* data);
    void Bind(lv_obj_t* object, AppTile& app);
    bool InitializeIcons();
    lv_obj_t* Icon(lv_obj_t* parent, int x, int y, int size, unsigned symbol);
    struct IconStorage;

    Callbacks callbacks_{};
    OrionTouch touch_;
    lv_display_t* display_{};
    lv_obj_t *home_{}, *guest_{}, *catalog_{}, *catalog_overlay_{};
    lv_obj_t *clock_card_{}, *weather_card_{}, *alarm_card_{}, *agenda_card_{}, *music_card_{}, *weather_button_{};
    lv_obj_t *time_{}, *date_{}, *alarm_{}, *alarm_status_{}, *agenda_date_{}, *count_{}, *status_{};
    lv_obj_t *weather_city_{}, *weather_value_{}, *weather_condition_{}, *weather_icon_{};
    lv_obj_t *music_title_{}, *music_status_{}, *music_play_{}, *music_next_{}, *music_play_background_{};
    lv_obj_t *music_play_icon_{}, *music_next_icon_{};
    lv_obj_t *text_panel_{}, *text_area_{};
    IconStorage* icons_{};
    micropixel_widget_snapshot_t weather_{};
    uint32_t weather_revision_{};
    lv_font_t music_font_{};
    host::WidgetSnapshot music_{};
    uint32_t music_revision_{};
    bool music_online_{}, music_pending_{};
    std::array<AppTile, host::AppManager::kMaxApps> apps_{};
    uint32_t count_apps_{};
    uint64_t last_minute_{UINT64_MAX}, last_alarm_{UINT64_MAX};
    int32_t last_offset_{};
    enum class TouchRoute { kHome, kGuest, kNavigation, kKeyboard };
    TouchRoute touch_route_{TouchRoute::kHome};
    bool front_pressed_{}, home_swipe_{};
};

}  // namespace micropixel::bouffalo
