// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "display/orion_touch.hpp"
#include "host/controller/app_hub.hpp"
#include "host/controller/app_manager.hpp"
#include "lvgl.h"

namespace micropixel::bouffalo {

class WeatherState;
class MediaWidgets;

enum class ManageAction : uint8_t { kOpen, kRemove, kConfirm, kCancel };

enum class HomeAppRole : uint8_t {
    kSettings,
    kClock,
    kWeather,
    kMusic,
    kNews,
    kLightSequence,
    kCentralAc,
    kDiagnostic
};

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
        void (*manage)(void*, ManageAction, uint32_t);
        void (*media_action)(void*, const host::WidgetSnapshot&, uint16_t, uint32_t);
    };

    [[nodiscard]] bool Initialize(Callbacks callbacks);
    void ClearApps();
    void AddApp(uint32_t index, const char* title, HomeAppRole role);
    void AddManagedApp(uint32_t index, const char* title, const char* version);
    void ManageStatus(const char* text, bool confirm, bool cancel);
    void ManageSpace(uint32_t count, uint32_t limit, size_t free_bytes);
    void Update(const micropixel_wall_time_t& wall);
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

    struct Shortcut final {
        lv_obj_t* card{};
        lv_obj_t* status{};
    };

    static void Launch(lv_event_t* event);
    static void Back(lv_event_t* event);
    static void ToggleCatalog(lv_event_t* event);
    static void OpenManagement(lv_event_t* event);
    static void CloseManagement(lv_event_t* event);
    static void RemoveManagedApp(lv_event_t* event);
    static void ConfirmManagement(lv_event_t* event);
    static void CancelManagement(lv_event_t* event);
    static void ChangeVolume(lv_event_t* event);
    static void TogglePlayback(lv_event_t* event);
    static void NextTrack(lv_event_t* event);
    static void KeyboardAction(lv_event_t* event);
    static void ReadTouch(lv_indev_t* pointer, lv_indev_data_t* data);
    void Bind(lv_obj_t* object, AppTile& app);
    void BindShortcut(const Shortcut& shortcut, AppTile& app);
    bool InitializeIcons();
    void InitializeManagement();
    lv_obj_t* Icon(lv_obj_t* parent, int x, int y, int size, unsigned symbol);
    struct IconStorage;

    Callbacks callbacks_{};
    OrionTouch touch_;
    lv_display_t* display_{};
    lv_obj_t *home_{}, *guest_{}, *catalog_{}, *catalog_overlay_{};
    lv_obj_t *manage_{}, *managed_apps_{}, *manage_space_{}, *manage_status_{}, *confirm_{}, *cancel_{};
    uint32_t managed_count_{};
    lv_obj_t *clock_card_{}, *weather_card_{}, *music_card_{}, *weather_button_{};
    Shortcut light_shortcut_{}, ac_shortcut_{};
    lv_obj_t *time_{}, *date_{}, *count_{}, *status_{};
    lv_obj_t *weather_city_{}, *weather_value_{}, *weather_condition_{}, *weather_icon_{};
    lv_obj_t *music_title_{}, *music_status_{}, *music_play_{}, *music_next_{}, *music_play_background_{};
    lv_obj_t *music_play_icon_{}, *music_next_icon_{};
    lv_obj_t *text_panel_{}, *text_area_{};
    IconStorage* icons_{};
    micropixel_widget_snapshot_t weather_{};
    uint32_t weather_revision_{};
    host::WidgetSnapshot music_{};
    uint32_t music_revision_{};
    bool music_online_{}, music_pending_{};
    std::array<AppTile, host::AppManager::kMaxApps> apps_{};
    uint32_t count_apps_{};
    uint64_t last_minute_{UINT64_MAX};
    int32_t last_offset_{};
    enum class TouchRoute { kHome, kGuest, kNavigation, kKeyboard };
    TouchRoute touch_route_{TouchRoute::kHome};
    bool front_pressed_{}, home_swipe_{};
};

}  // namespace micropixel::bouffalo
