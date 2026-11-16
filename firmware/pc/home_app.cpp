// SPDX-License-Identifier: Apache-2.0
#include "home_app.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "app_sessions.hpp"
#include "esp_timer.h"
#include "sdk/ui/material_icons_data.hpp"
namespace micropixel::pc {
namespace {
constexpr uint32_t kBg = 0x101214, kSurface = 0x1b1f23, kRaised = 0x242a2f, kText = 0xf5f7f8, kMuted = 0x8c979e,
                   kAmber = 0xe8a15c;
enum ActionId {
    kMusic,
    kMarket,
    kResume,
    kPause,
    kClose,
    kVolume,
    kNext,
    kApps,
    kCalendar,
    kClock,
    kWeather,
    kRefresh
};
void Panel(lv_obj_t* object, uint32_t color, int radius = 20) {
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_scrollable(object, false);
    lv_obj_set_style_shadow_width(object, 0, 0);
}
lv_obj_t* Label(lv_obj_t* parent, int x, int y, int width, const char* text, uint32_t color) {
    auto* label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}
lv_obj_t* Button(lv_obj_t* parent, int x, int y, int width, int height, const char* text, int action,
                 lv_event_cb_t callback, void* context, bool accent = false) {
    auto* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, width, height);
    Panel(b, accent ? kAmber : kSurface, 16);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent ? kAmber : kSurface), LV_STATE_DISABLED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
    auto* label = Label(b, 0, 0, width - 8, text, accent ? kBg : kText);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(b, callback, LV_EVENT_SHORT_CLICKED, context);
    return b;
}
lv_obj_t* Symbol(lv_obj_t* parent, int x, int y, int size, const char* symbol, uint32_t background,
                 uint32_t foreground) {
    auto* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, size, size);
    Panel(obj, background, LV_RADIUS_CIRCLE);
    lv_obj_set_clickable(obj, false);
    auto* label = lv_label_create(obj);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(foreground), 0);
    lv_obj_center(label);
    return obj;
}
void Local(uint64_t utc, int32_t offset, std::tm& out) {
    auto t = static_cast<time_t>(static_cast<int64_t>(utc) + int64_t(offset) * 60);
    gmtime_r(&t, &out);
}
}  // namespace
HomeApp::HomeApp(std::vector<std::unique_ptr<PortableApp>>& apps, MediaPlayer& media, SdlAudio& audio,
                 AppSessions* sessions, HomeFonts fonts)
    : apps_(apps), media_(&media), audio_(audio), sessions_(sessions), fonts_(fonts) {}
HomeApp::~HomeApp() { Hide(); }
void HomeApp::Back() {
    if (catalog_overlay_ && !lv_obj_is_hidden(catalog_overlay_)) {
        lv_obj_set_hidden(catalog_overlay_, true);
        std::fputs("HOME catalog closed\n", stderr);
    }
}
void HomeApp::WidgetEvent(lv_event_t* event) {
    auto& self = *static_cast<HomeApp*>(lv_event_get_user_data(event));
    auto index = uint32_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    if (index < 8 && self.widget_valid_[index]) self.requested = self.widgets_[index].app;
}
void HomeApp::Launch(lv_event_t* event) {
    auto& self = *static_cast<HomeApp*>(lv_event_get_user_data(event));
    self.requested = int(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
}
void HomeApp::SendWidget(const host::WidgetSnapshot& widget, uint16_t type, uint32_t value) {
    if (!sessions_ || widget_ticket_) return;
    uint32_t ticket = 0;
    auto status = sessions_->WidgetAction(widget, type, value, sessions_->NextHomeRequest(), ticket);
    if (status) {
        std::fprintf(stderr, "HOME_WIDGET rejected: app=%u type=%u status=%d\n", widget.app, type, status);
        return;
    }
    widget_ticket_ = ticket;
    pending_type_ = type;
    pending_value_ = value;
    updated_at_ = 0;
    std::fprintf(stderr, "HOME_WIDGET pending: app=%u type=%u ticket=%u\n", widget.app, type, ticket);
}
void HomeApp::OpenClock(uint32_t page) {
    if (clock_widget_ < 8) {
        SendWidget(widgets_[clock_widget_], MICROPIXEL_ACTION_OPEN_PAGE, page);
        requested = widgets_[clock_widget_].app;
    } else
        for (uint32_t i = 0; i < apps_.size(); ++i)
            if (apps_[i]->available && apps_[i]->app_id == "micropixel.clock-alarm") requested = int(i);
}
void HomeApp::Action(lv_event_t* event) {
    auto& self = *static_cast<HomeApp*>(lv_event_get_user_data(event));
    switch (int(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))))) {
        case kMusic:
            self.music_requested = true;
            break;
        case kMarket:
            self.market_requested = true;
            break;
        case kApps: {
            bool show = lv_obj_is_hidden(self.catalog_overlay_);
            lv_obj_set_hidden(self.catalog_overlay_, !show);
            if (show) lv_obj_move_foreground(self.catalog_overlay_);
            std::fprintf(stderr, "HOME catalog %s\n", show ? "opened" : "closed");
            break;
        }
        case kCalendar:
            self.OpenClock(1);
            break;
        case kClock:
            self.OpenClock(0);
            break;
        case kWeather:
            if (self.weather_widget_ < 8)
                self.requested = self.widgets_[self.weather_widget_].app;
            else
                for (uint32_t i = 0; i < self.apps_.size(); ++i)
                    if (self.apps_[i]->available && self.apps_[i]->app_id == "micropixel.weather")
                        self.requested = int(i);
            break;
        case kRefresh:
            if (self.weather_widget_ < 8)
                self.SendWidget(self.widgets_[self.weather_widget_], MICROPIXEL_ACTION_REFRESH, 0);
            break;
        case kResume:
            if (self.media_widget_ < 8)
                self.requested = self.widgets_[self.media_widget_].app;
            else
                self.music_requested = true;
            break;
        case kPause:
            if (self.media_widget_ < 8) {
                auto& w = self.widgets_[self.media_widget_];
                self.SendWidget(w, MICROPIXEL_ACTION_SET_PLAYING, !(w.data.flags & MICROPIXEL_WIDGET_PLAYING));
            } else {
                self.media_->Pause(!self.media_->Paused());
                std::fprintf(stderr, "HOME_MEDIA paused=%d position_ms=%.0f\n", self.media_->Paused(),
                             self.media_->PositionSeconds() * 1000);
            }
            break;
        case kNext:
            if (self.media_widget_ < 8)
                self.SendWidget(self.widgets_[self.media_widget_], MICROPIXEL_ACTION_NEXT_TRACK, 0);
            break;
        case kClose:
            if (self.running_)
                self.close_requested = true;
            else if (self.media_widget_ < 8)
                self.stop_requested = self.widgets_[self.media_widget_].app;
            break;
        case kVolume:
            self.audio_.SetMasterVolume(lv_slider_get_value(lv_event_get_target_obj(event)));
            break;
    }
}
void HomeApp::UpdateWidgets() {
    media_widget_ = weather_widget_ = clock_widget_ = UINT32_MAX;
    for (uint32_t i = 0; i < 8; ++i) {
        widget_valid_[i] = sessions_ && sessions_->Widget(i, widgets_[i]);
        if (!widget_valid_[i]) continue;
        const auto& w = widgets_[i];
        if (w.revision != logged_revision_[i] || w.online != logged_online_[i]) {
            std::fprintf(stderr, "HOME_WIDGET snapshot: app=%u template=%u revision=%u online=%d index=%u\n", w.app,
                         w.data.template_id, w.revision, w.online, i);
            logged_revision_[i] = w.revision;
            logged_online_[i] = w.online;
        }
        if (w.data.template_id == MICROPIXEL_WIDGET_MEDIA &&
            (media_widget_ >= 8 || (w.app < apps_.size() && apps_[w.app].get() == running_)))
            media_widget_ = i;
        if (w.data.template_id == MICROPIXEL_WIDGET_WEATHER && weather_widget_ >= 8) weather_widget_ = i;
        if (w.data.template_id == MICROPIXEL_WIDGET_CLOCK && clock_widget_ >= 8) clock_widget_ = i;
    }
    if (weather_widget_ < 8) {
        const auto& w = widgets_[weather_widget_];
        lv_label_set_text(weather_city_, w.data.title);
        char temp[32]{};
        if (w.data.updated_utc)
            std::snprintf(temp, sizeof(temp), w.data.value % 10 ? "%.1f°" : "%.0f°", double(w.data.value) / 10);
        else
            std::snprintf(temp, sizeof(temp), "--°");
        lv_label_set_text(weather_value_, temp);
        auto* separator = std::strstr(w.data.subtitle, "·");
        lv_label_set_text(weather_condition_, (w.data.flags & MICROPIXEL_WIDGET_ERROR) ? "刷新失败 · 显示上次天气"
                                              : !w.data.updated_utc                    ? "等待天气更新"
                                              : separator                              ? separator + std::strlen("·")
                                                                                       : w.data.subtitle);
        unsigned icon = w.data.updated_utc ? micropixel::ui::material_art::Index(w.data.auxiliary) : 8;
        lv_obj_set_pos(weather_icon_, -int(icon % micropixel::ui::material_art::Columns) * 54,
                       -int(icon / micropixel::ui::material_art::Columns) * 54);
    }
    lv_obj_set_state(weather_refresh_, LV_STATE_DISABLED, weather_widget_ >= 8 || widget_ticket_);
}
void HomeApp::UpdateClock() {
    auto wall = sessions_ ? sessions_->Clock() : micropixel_wall_time_t{};
    if (!sessions_) {
        wall.utc = std::time(nullptr);
        wall.offset_minutes = 480;
        wall.valid = 1;
    }
    if (wall.utc == clock_second_) return;
    clock_second_ = wall.utc;
    std::tm local{};
    Local(wall.utc, wall.offset_minutes, local);
    constexpr const char* week[]{"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char date[96]{}, time[32]{};
    std::snprintf(date, sizeof(date), "%d月%d日  %s", local.tm_mon + 1, local.tm_mday, week[local.tm_wday]);
    std::snprintf(time, sizeof(time), "%02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(date_, wall.valid ? date : "请同步时间");
    lv_label_set_text(time_, wall.valid ? time : "--:--");
}
void HomeApp::SetRunning(const PortableApp* app) {
    if (running_ == app) return;
    running_ = app;
    updated_at_ = 0;
    Update();
}
lv_obj_t* HomeApp::Icon(lv_obj_t* parent, int x, int y, int size, unsigned symbol) {
    namespace art = micropixel::ui::material_art;
    auto* clip = lv_obj_create(parent);
    lv_obj_set_pos(clip, x, y);
    lv_obj_set_size(clip, size, size);
    Panel(clip, kBg, 0);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(clip, false);
    auto* icon = lv_image_create(clip);
    lv_image_set_src(icon, &weather_image_);
    lv_image_set_pivot(icon, 0, 0);
    lv_image_set_scale(icon, size * 256 / art::Cell);
    lv_obj_set_pos(icon, -int(symbol % art::Columns) * size, -int(symbol / art::Columns) * size);
    return icon;
}
void HomeApp::Show(int width, int height) {
    Hide();
    width_ = width;
    height_ = height;
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root_, 720, 720);
    Panel(root_, kBg, 0);
    if (fonts_.body) lv_obj_set_style_text_font(root_, fonts_.body, 0);
    auto scale = std::min(width, height) * 256 / 720;
    lv_obj_set_style_transform_scale_x(root_, scale, 0);
    lv_obj_set_style_transform_scale_y(root_, scale, 0);
    lv_obj_set_style_transform_pivot_x(root_, 0, 0);
    lv_obj_set_style_transform_pivot_y(root_, 0, 0);
    lv_obj_set_pos(root_, (width - 720 * scale / 256) / 2, (height - 720 * scale / 256) / 2);
    Symbol(root_, 24, 27, 9, "", kAmber, kAmber);
    Label(root_, 48, 21, 380, "家中 · MicroPixel", kMuted);
    auto* hero = lv_obj_create(root_);
    lv_obj_set_pos(hero, 24, 75);
    lv_obj_set_size(hero, 672, 198);
    Panel(hero, 0x232a30, 30);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0x191d21), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);
    auto* clock_hit = Button(root_, 24, 75, 438, 198, "", kClock, Action, this);
    lv_obj_set_style_bg_opa(clock_hit, LV_OPA_TRANSP, 0);
    time_ = Label(root_, 54, 90, 430, "--:--", kText);
    lv_obj_set_style_text_font(time_, fonts_.clock ? fonts_.clock : &lv_font_montserrat_48, 0);
    date_ = Label(root_, 54, 228, 420, " ", kMuted);
    weather_city_ = Label(root_, 504, 85, 150, "天气", kMuted);
    weather_value_ = Label(root_, 504, 148, 180, "--°", kText);
    lv_obj_set_style_text_font(weather_value_, fonts_.temperature ? fonts_.temperature : &lv_font_montserrat_48, 0);
    weather_condition_ = Label(root_, 504, 233, 174, "等待更新", kMuted);
    namespace art = micropixel::ui::material_art;
    if (weather_pixels_.empty()) {
        weather_pixels_.resize(art::Width * art::Height * 4);
        (void)art::Expand(weather_pixels_);
    }
    weather_image_ = {};
    weather_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    weather_image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    weather_image_.header.w = art::Width;
    weather_image_.header.h = art::Height;
    weather_image_.header.stride = art::Width * 4;
    weather_image_.data_size = weather_pixels_.size();
    weather_image_.data = weather_pixels_.data();
    auto* weather_clip = lv_obj_create(root_);
    lv_obj_set_pos(weather_clip, 612, 105);
    lv_obj_set_size(weather_clip, 54, 54);
    Panel(weather_clip, kBg, 0);
    lv_obj_set_style_bg_opa(weather_clip, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(weather_clip, false);
    weather_icon_ = lv_image_create(weather_clip);
    lv_image_set_src(weather_icon_, &weather_image_);
    lv_image_set_pivot(weather_icon_, 0, 0);
    lv_image_set_scale(weather_icon_, 54 * 256 / art::Cell);
    lv_obj_set_pos(weather_icon_, -int(8 % art::Columns) * 54, -int(8 / art::Columns) * 54);
    auto* weather_hit = Button(root_, 468, 75, 228, 198, "", kWeather, Action, this);
    lv_obj_set_style_bg_opa(weather_hit, LV_OPA_TRANSP, 0);
    weather_refresh_ = Button(root_, 612, 0, 84, 75, "", kRefresh, Action, this);
    lv_obj_set_style_bg_opa(weather_refresh_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(weather_refresh_, LV_OPA_TRANSP, LV_STATE_DISABLED);
    Icon(weather_refresh_, 25, 20, 34, unsigned(micropixel::ui::material_art::Symbol::Refresh));
    auto* alarm = Button(root_, 24, 291, 327, 174, "", kClock, Action, this);
    lv_obj_set_style_radius(alarm, 27, 0);
    Label(alarm, 24, 21, 210, "闹钟", kMuted);
    Icon(alarm, 255, 24, 34, unsigned(micropixel::ui::material_art::Symbol::Alarm));
    alarm_time_ = Label(alarm, 24, 69, 279, "--:--", kText);
    lv_obj_set_style_text_font(alarm_time_, &lv_font_montserrat_48, 0);
    alarm_subtitle_ = Label(alarm, 24, 137, 279, "设置一个提醒", kMuted);
    auto* agenda = Button(root_, 369, 291, 327, 174, "", kCalendar, Action, this);
    lv_obj_set_style_radius(agenda, 27, 0);
    Label(agenda, 24, 21, 279, "日历与事项", kMuted);
    Icon(agenda, 255, 24, 34, unsigned(micropixel::ui::material_art::Symbol::Calendar));
    agenda_title_ = Label(agenda, 24, 76, 279, "今天", kText);
    if (fonts_.value) lv_obj_set_style_text_font(agenda_title_, fonts_.value, 0);
    agenda_subtitle_ = Label(agenda, 24, 137, 279, "暂无待提醒事项", kMuted);
    auto* current = lv_obj_create(root_);
    lv_obj_set_pos(current, 24, 483);
    lv_obj_set_size(current, 327, 174);
    Panel(current, kSurface, 27);
    resume_ = Button(current, 0, 0, 327, 174, "", kResume, Action, this);
    lv_obj_set_style_bg_opa(resume_, LV_OPA_TRANSP, 0);
    Label(current, 24, 17, 279, "音乐", kMuted);
    running_title_ = Label(current, 24, 51, 279, "网易云音乐", kText);
    status_ = Label(current, 24, 120, 112, "选择音乐", kMuted);
    pause_ = Button(current, 141, 76, 84, 84, "", kPause, Action, this, true);
    lv_obj_set_style_radius(pause_, 42, 0);
    pause_icon_ = Icon(pause_, 25, 25, 34, unsigned(micropixel::ui::material_art::Symbol::Play));
    lv_obj_set_style_image_recolor(pause_icon_, lv_color_hex(0x1a1305), 0);
    lv_obj_set_style_image_recolor_opa(pause_icon_, LV_OPA_COVER, 0);
    next_ = Button(current, 225, 76, 84, 84, "", kNext, Action, this);
    lv_obj_set_style_radius(next_, 42, 0);
    Icon(next_, 25, 25, 34, unsigned(micropixel::ui::material_art::Symbol::Next));
    auto* apps = Button(root_, 369, 483, 327, 174, "", kApps, Action, this);
    lv_obj_set_style_radius(apps, 27, 0);
    Label(apps, 24, 21, 279, "应用", kMuted);
    Icon(apps, 255, 24, 34, unsigned(micropixel::ui::material_art::Symbol::Apps));
    apps_count_ = Label(apps, 24, 68, 279, " ", kText);
    lv_obj_set_style_text_font(apps_count_, &lv_font_montserrat_48, 0);
    Label(apps, 24, 137, 279, "全部应用与系统音量", kMuted);
    auto* page = lv_obj_create(root_);
    lv_obj_set_pos(page, 344, 691);
    lv_obj_set_size(page, 24, 8);
    Panel(page, kAmber, 5);
    auto* page2 = lv_obj_create(root_);
    lv_obj_set_pos(page2, 377, 691);
    lv_obj_set_size(page2, 8, 8);
    Panel(page2, 0x3a4248, 4);
    catalog_overlay_ = lv_obj_create(root_);
    lv_obj_set_size(catalog_overlay_, 720, 720);
    Panel(catalog_overlay_, kBg, 0);
    Label(catalog_overlay_, 24, 25, 300, "全部应用", kText);
    Button(catalog_overlay_, 360, 0, 252, 84, "应用市场", kMarket, Action, this);
    auto* catalog_back = Button(catalog_overlay_, 612, 0, 84, 84, "", kApps, Action, this);
    Icon(catalog_back, 25, 25, 34, unsigned(art::Symbol::Close));
    catalog_ = lv_obj_create(catalog_overlay_);
    lv_obj_set_pos(catalog_, 24, 108);
    lv_obj_set_size(catalog_, 672, 480);
    Panel(catalog_, kBg, 0);
    lv_obj_set_scrollable(catalog_, true);
    lv_obj_set_scroll_dir(catalog_, LV_DIR_VER);
    for (uint32_t i = 0; i < apps_.size(); ++i) {
        if (!apps_[i]->available) continue;
        auto* b = Button(catalog_, int(i % 2) * 348, int(i / 2) * 116, 324, 104, "", i, Launch, this);
        lv_obj_set_style_radius(b, 24, 0);
        Symbol(b, 16, 24, 52, "", kRaised, kAmber);
        auto symbol = apps_[i]->app_id == "micropixel.netease-music" ? art::Symbol::Music
                      : apps_[i]->app_id == "micropixel.weather"     ? art::Symbol::PartlyCloudy
                      : apps_[i]->app_id == "micropixel.clock-alarm" ? art::Symbol::Calendar
                                                                     : art::Symbol::Apps;
        Icon(b, 25, 33, 34, unsigned(symbol));
        Label(b, 84, 22, 224, apps_[i]->title.c_str(), kText);
        Label(b, 84, 56, 224, running_ == apps_[i].get() ? "返回应用" : "点击打开", kMuted);
    }
    Label(catalog_overlay_, 24, 642, 88, "音量", kMuted);
    auto* volume = lv_slider_create(catalog_overlay_);
    lv_obj_set_pos(volume, 120, 642);
    lv_obj_set_size(volume, 264, 12);
    lv_obj_set_ext_click_area(volume, 36);
    lv_slider_set_range(volume, 0, 100);
    lv_slider_set_value(volume, audio_.MasterVolume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume, lv_color_hex(kAmber), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume, lv_color_hex(kText), LV_PART_KNOB);
    lv_obj_set_user_data(volume, reinterpret_cast<void*>(kVolume));
    lv_obj_add_event_cb(volume, Action, LV_EVENT_VALUE_CHANGED, this);
    close_ = Button(catalog_overlay_, 444, 612, 252, 84, "关闭当前应用", kClose, Action, this);
    lv_obj_update_layout(catalog_);
    lv_obj_scroll_to_y(catalog_, scroll_y_, LV_ANIM_OFF);
    lv_obj_set_hidden(catalog_overlay_, true);
    clock_second_ = UINT64_MAX;
    updated_at_ = 0;
    Update();
    std::fprintf(stderr, "HOME visible: running=%s apps=%zu\n", running_ ? running_->app_id.c_str() : "none",
                 apps_.size());
}
void HomeApp::Update() {
    if (!root_) return;
    if (widget_ticket_ && sessions_) {
        auto result = sessions_->WidgetResult(widget_ticket_);
        if (result.complete) {
            std::fprintf(stderr, "HOME_WIDGET result: ticket=%u status=%d\n", widget_ticket_, result.result);
            if (!result.result && pending_type_ == MICROPIXEL_ACTION_SET_PLAYING)
                std::fprintf(stderr, "HOME_MEDIA paused=%u position_ms=0\n", pending_value_ ? 0 : 1);
            widget_ticket_ = 0;
            updated_at_ = 0;
        }
    }
    auto now = esp_timer_get_time();
    if (updated_at_ && now - updated_at_ < 250000) return;
    updated_at_ = now;
    UpdateWidgets();
    UpdateClock();
    if (sessions_) {
        auto summary = sessions_->Planner();
        auto wall = sessions_->Clock();
        std::tm local{};
        char text[80]{};
        if (summary.alarm_utc) {
            Local(summary.alarm_utc, wall.offset_minutes, local);
            std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
        } else
            std::snprintf(text, sizeof(text), "--:--");
        lv_label_set_text(alarm_time_, text);
        lv_label_set_text(alarm_subtitle_, summary.alarm_utc ? "下次提醒" : "尚未设置");
        if (summary.note_utc) {
            Local(summary.note_utc, wall.offset_minutes, local);
            lv_label_set_text(agenda_title_, summary.note.data());
            std::snprintf(text, sizeof(text), "%d/%d %02d:%02d · %u项", local.tm_mon + 1, local.tm_mday, local.tm_hour,
                          local.tm_min, summary.pending_notes);
            lv_label_set_text(agenda_subtitle_, text);
        } else {
            char today[32]{};
            Local(wall.utc, wall.offset_minutes, local);
            std::snprintf(today, sizeof(today), "%d月%d日", local.tm_mon + 1, local.tm_mday);
            lv_label_set_text(agenda_title_, today);
            lv_label_set_text(agenda_subtitle_, "暂无待提醒事项");
        }
    }
    char status[160]{};
    bool playing = false, active = false;
    if (media_widget_ < 8) {
        auto& w = widgets_[media_widget_];
        playing = w.data.flags & MICROPIXEL_WIDGET_PLAYING;
        active = w.online;
        lv_label_set_text(running_title_, w.data.title);
        std::snprintf(status, sizeof(status), "%s · %02u:%02u / %02u:%02u",
                      !w.online ? "应用已停止"
                      : playing ? "正在播放"
                                : "已暂停",
                      w.data.position_ms / 60000, w.data.position_ms / 1000 % 60, w.data.duration_ms / 60000,
                      w.data.duration_ms / 1000 % 60);
    } else {
        active = !media_->Finished() && !media_->Failed();
        playing = active && !media_->Paused();
        lv_label_set_text(running_title_, "网易云音乐");
        std::snprintf(status, sizeof(status), "%s", active ? (playing ? "正在播放" : "已暂停") : "听一首喜欢的歌");
    }
    lv_label_set_text(status_, !active ? "选择音乐" : playing ? "正在播放" : "已暂停");
    char count[24]{};
    std::snprintf(count, sizeof(count), "%zu", apps_.size());
    lv_label_set_text(apps_count_, count);
    unsigned play_icon =
        unsigned(playing ? micropixel::ui::material_art::Symbol::Pause : micropixel::ui::material_art::Symbol::Play);
    lv_obj_set_pos(pause_icon_, -int(play_icon % micropixel::ui::material_art::Columns) * 34,
                   -int(play_icon / micropixel::ui::material_art::Columns) * 34);
    lv_obj_set_state(pause_, LV_STATE_DISABLED, !active || widget_ticket_);
    lv_obj_set_state(next_, LV_STATE_DISABLED, media_widget_ >= 8 || !active || widget_ticket_);
    lv_obj_set_state(close_, LV_STATE_DISABLED, !running_ && media_widget_ >= 8);
}
void HomeApp::Hide() {
    if (catalog_) scroll_y_ = lv_obj_get_scroll_y(catalog_);
    if (root_) lv_obj_delete(root_);
    root_ = catalog_ = catalog_overlay_ = nullptr;
    updated_at_ = 0;
}
}  // namespace micropixel::pc
