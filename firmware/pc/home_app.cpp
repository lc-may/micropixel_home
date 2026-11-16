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
namespace micropixel::pc {
namespace {
constexpr uint32_t kBg = 0x15181d, kSurface = 0x292d34, kRaised = 0x34383f, kText = 0xf5f6f8, kMuted = 0x9fa6b3,
                   kAmber = 0xf5ae39;
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
                 AppSessions* sessions)
    : apps_(apps), media_(&media), audio_(audio), sessions_(sessions) {}
HomeApp::~HomeApp() { Hide(); }
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
        lv_obj_set_hidden(weather_sun_, w.data.auxiliary >= 3 || !w.data.updated_utc);
        lv_obj_set_hidden(weather_rain_, w.data.auxiliary < 50 || !w.data.updated_utc);
        lv_obj_set_style_bg_opa(weather_cloud_, w.data.updated_utc ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_style_bg_opa(weather_bump_, w.data.updated_utc ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_set_hidden(weather_cloud_, w.data.updated_utc && w.data.auxiliary == 0);
        lv_obj_set_hidden(weather_bump_, w.data.updated_utc && w.data.auxiliary == 0);
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
    const double angles[]{(local.tm_hour % 12 + local.tm_min / 60.0) * 6.283185307 / 12,
                          local.tm_min * 6.283185307 / 60.0, local.tm_sec * 6.283185307 / 60.0};
    const int lengths[]{76, 110, 119};
    for (unsigned i = 0; i < 3; ++i) {
        auto a = angles[i] - 1.570796327;
        hand_points_[i][0] = {150 - int(std::cos(a) * 18), 150 - int(std::sin(a) * 18)};
        hand_points_[i][1] = {150 + int(std::cos(a) * lengths[i]), 150 + int(std::sin(a) * lengths[i])};
        lv_line_set_points(hands_[i], hand_points_[i].data(), 2);
    }
}
void HomeApp::SetRunning(const PortableApp* app) {
    if (running_ == app) return;
    running_ = app;
    updated_at_ = 0;
    Update();
}
void HomeApp::Show(int width, int height) {
    Hide();
    width_ = width;
    height_ = height;
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root_, 720, 720);
    Panel(root_, kBg, 0);
    lv_obj_set_style_bg_grad_color(root_, lv_color_hex(0x34352f), 0);
    lv_obj_set_style_bg_grad_dir(root_, LV_GRAD_DIR_HOR, 0);
    auto scale = std::min(width, height) * 256 / 720;
    lv_obj_set_style_transform_scale_x(root_, scale, 0);
    lv_obj_set_style_transform_scale_y(root_, scale, 0);
    lv_obj_set_style_transform_pivot_x(root_, 0, 0);
    lv_obj_set_style_transform_pivot_y(root_, 0, 0);
    lv_obj_set_pos(root_, (width - 720 * scale / 256) / 2, (height - 720 * scale / 256) / 2);
    Label(root_, 28, 22, 380, "MicroPixel  ·  我的家", kMuted);
    clock_face_ = lv_button_create(root_);
    lv_obj_set_pos(clock_face_, 52, 76);
    lv_obj_set_size(clock_face_, 300, 300);
    Panel(clock_face_, 0xf1f0e5, LV_RADIUS_CIRCLE);
    lv_obj_set_user_data(clock_face_, reinterpret_cast<void*>(kClock));
    lv_obj_add_event_cb(clock_face_, Action, LV_EVENT_SHORT_CLICKED, this);
    for (unsigned i = 0; i < 60; ++i) {
        double angle = i * 6.283185307 / 60 - 1.570796327;
        int inner = i % 5 ? 135 : 125;
        ticks_[i][0] = {150 + int(std::cos(angle) * inner), 150 + int(std::sin(angle) * inner)};
        ticks_[i][1] = {150 + int(std::cos(angle) * 142), 150 + int(std::sin(angle) * 142)};
        auto* line = lv_line_create(clock_face_);
        lv_line_set_points(line, ticks_[i].data(), 2);
        lv_obj_set_style_line_color(line, lv_color_hex(0x333733), 0);
        lv_obj_set_style_line_width(line, i % 5 ? 2 : 5, 0);
    }
    for (unsigned i = 0; i < 3; ++i) {
        hands_[i] = lv_line_create(clock_face_);
        lv_obj_set_style_line_color(hands_[i], lv_color_hex(i == 2 ? 0xee4056 : 0x30352e), 0);
        lv_obj_set_style_line_width(hands_[i], i == 0 ? 9 : i == 1 ? 6 : 2, 0);
        lv_obj_set_style_line_rounded(hands_[i], true, 0);
    }
    Symbol(clock_face_, 143, 143, 14, "", 0xee4056, 0xee4056);
    time_ = Label(root_, 143, 384, 180, "--:--", kText);
    lv_obj_set_style_text_font(time_, &lv_font_montserrat_24, 0);
    weather_city_ = Label(root_, 428, 82, 180, "天气", kMuted);
    weather_refresh_ = Button(root_, 628, 74, 64, 44, "刷新", kRefresh, Action, this);
    weather_sun_ = lv_obj_create(root_);
    lv_obj_set_pos(weather_sun_, 518, 138);
    lv_obj_set_size(weather_sun_, 78, 78);
    Panel(weather_sun_, kAmber, LV_RADIUS_CIRCLE);
    weather_cloud_ = lv_obj_create(root_);
    lv_obj_set_pos(weather_cloud_, 446, 184);
    lv_obj_set_size(weather_cloud_, 154, 58);
    Panel(weather_cloud_, 0xf1f3f4, 29);
    weather_bump_ = lv_obj_create(root_);
    lv_obj_set_pos(weather_bump_, 474, 156);
    lv_obj_set_size(weather_bump_, 82, 82);
    Panel(weather_bump_, 0xf1f3f4, LV_RADIUS_CIRCLE);
    weather_rain_ = Label(root_, 470, 246, 140, "/  /  /", 0x79beda);
    lv_obj_set_hidden(weather_rain_, true);
    weather_value_ = Label(root_, 430, 270, 262, "--°", kText);
    lv_obj_set_style_text_font(weather_value_, &lv_font_montserrat_48, 0);
    weather_condition_ = Label(root_, 430, 330, 262, "等待天气更新", kMuted);
    date_ = Label(root_, 430, 366, 262, " ", kText);
    auto* weather_open = Button(root_, 412, 122, 284, 282, "", kWeather, Action, this);
    lv_obj_set_style_bg_opa(weather_open, LV_OPA_TRANSP, 0);
    auto* alarm = Button(root_, 24, 428, 324, 116, "", kClock, Action, this);
    Symbol(alarm, 18, 26, 60, LV_SYMBOL_BELL, kAmber, kBg);
    Label(alarm, 96, 18, 208, "闹钟", kMuted);
    alarm_time_ = Label(alarm, 96, 54, 212, "未设置", kText);
    lv_obj_set_style_text_font(alarm_time_, &lv_font_montserrat_32, 0);
    auto* agenda = Button(root_, 372, 428, 324, 116, "", kCalendar, Action, this);
    Symbol(agenda, 18, 26, 60, LV_SYMBOL_LIST, kRaised, kText);
    agenda_title_ = Label(agenda, 96, 18, 208, "日历与事项", kText);
    agenda_subtitle_ = Label(agenda, 96, 58, 208, "添加一个重要提醒", kMuted);
    auto* current = lv_obj_create(root_);
    lv_obj_set_pos(current, 24, 564);
    lv_obj_set_size(current, 672, 84);
    Panel(current, kSurface);
    Symbol(current, 14, 16, 52, LV_SYMBOL_AUDIO, kRaised, kAmber);
    running_title_ = Label(current, 82, 12, 390, "网易云音乐", kText);
    status_ = Label(current, 82, 46, 390, "听一首喜欢的歌", kMuted);
    resume_ = Button(current, 0, 0, 468, 84, "", kResume, Action, this);
    lv_obj_set_style_bg_opa(resume_, LV_OPA_TRANSP, 0);
    pause_ = Button(current, 492, 10, 64, 64, LV_SYMBOL_PLAY, kPause, Action, this, true);
    lv_obj_set_style_radius(pause_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(pause_, 0), &lv_font_montserrat_24, 0);
    next_ = Button(current, 580, 10, 64, 64, LV_SYMBOL_NEXT, kNext, Action, this);
    lv_obj_set_style_text_font(lv_obj_get_child(next_, 0), &lv_font_montserrat_24, 0);
    Button(root_, 24, 666, 100, 40, "日历", kCalendar, Action, this);
    Button(root_, 136, 666, 100, 40, "天气", kWeather, Action, this);
    Button(root_, 248, 666, 100, 40, "音乐", kMusic, Action, this);
    Button(root_, 360, 666, 100, 40, "应用", kApps, Action, this);
    Label(root_, 478, 676, 48, "音量", kMuted);
    auto* volume = lv_slider_create(root_);
    lv_obj_set_pos(volume, 530, 682);
    lv_obj_set_size(volume, 72, 6);
    lv_slider_set_range(volume, 0, 100);
    lv_slider_set_value(volume, audio_.MasterVolume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume, lv_color_hex(kAmber), LV_PART_INDICATOR);
    lv_obj_set_user_data(volume, reinterpret_cast<void*>(kVolume));
    lv_obj_add_event_cb(volume, Action, LV_EVENT_VALUE_CHANGED, this);
    close_ = Button(root_, 616, 666, 80, 40, "关闭", kClose, Action, this);
    catalog_overlay_ = lv_obj_create(root_);
    lv_obj_set_size(catalog_overlay_, 720, 720);
    Panel(catalog_overlay_, kBg, 0);
    Label(catalog_overlay_, 24, 28, 280, "全部应用", kText);
    Button(catalog_overlay_, 424, 18, 160, 48, "应用市场", kMarket, Action, this);
    Button(catalog_overlay_, 608, 18, 88, 48, "返回", kApps, Action, this);
    catalog_ = lv_obj_create(catalog_overlay_);
    lv_obj_set_pos(catalog_, 24, 130);
    lv_obj_set_size(catalog_, 672, 562);
    Panel(catalog_, kBg, 0);
    lv_obj_set_scrollable(catalog_, true);
    lv_obj_set_scroll_dir(catalog_, LV_DIR_VER);
    for (uint32_t i = 0; i < apps_.size(); ++i) {
        if (!apps_[i]->available) continue;
        auto* b = Button(catalog_, int(i % 2) * 348, int(i / 2) * 116, 324, 104, "", i, Launch, this);
        Symbol(b, 16, 24, 52, LV_SYMBOL_FILE, kRaised, kAmber);
        Label(b, 84, 22, 224, apps_[i]->title.c_str(), kText);
        Label(b, 84, 56, 224, running_ == apps_[i].get() ? "返回应用" : "点击打开", kMuted);
    }
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
        if (summary.note_utc) {
            Local(summary.note_utc, wall.offset_minutes, local);
            lv_label_set_text(agenda_title_, summary.note.data());
            std::snprintf(text, sizeof(text), "%d/%d %02d:%02d · %u项", local.tm_mon + 1, local.tm_mday, local.tm_hour,
                          local.tm_min, summary.pending_notes);
            lv_label_set_text(agenda_subtitle_, text);
        } else {
            lv_label_set_text(agenda_title_, "日历与事项");
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
    lv_label_set_text(status_, status);
    lv_label_set_text(lv_obj_get_child(pause_, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
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
