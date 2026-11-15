// SPDX-License-Identifier: Apache-2.0
#include "home_app.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "app_sessions.hpp"
#include "esp_timer.h"

namespace micropixel::pc {
namespace {
enum ActionId { kMusic, kMarket, kResume, kPause, kClose, kVolume, kNext };
void Panel(lv_obj_t* object, uint32_t color, int radius = 18) {
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
                 lv_event_cb_t callback, void* context) {
    auto* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    Panel(button, 0x263a55, 14);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
    auto* label = Label(button, 0, 0, width - 8, text, 0xf2f6fc);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, context);
    return button;
}
}  // namespace
HomeApp::HomeApp(std::vector<std::unique_ptr<PortableApp>>& apps, MediaPlayer& media, SdlAudio& audio,
                 AppSessions* sessions)
    : apps_(apps), media_(&media), audio_(audio), sessions_(sessions) {}
HomeApp::~HomeApp() { Hide(); }
void HomeApp::WidgetEvent(lv_event_t* event) {
    auto& home = *static_cast<HomeApp*>(lv_event_get_user_data(event));
    const auto tag =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    const auto index = tag >= 100 ? tag - 100 : tag;
    if (index >= 8 || !home.widget_valid_[index]) return;
    if (tag >= 100)
        home.SendWidget(home.widgets_[index], MICROPIXEL_ACTION_REFRESH, 0);
    else
        home.requested = static_cast<int>(home.widgets_[index].app);
}
void HomeApp::SendWidget(const host::WidgetSnapshot& widget, uint16_t type, uint32_t value) {
    if (!sessions_ || widget_ticket_) return;
    uint32_t ticket = 0;
    const auto status = sessions_->WidgetAction(widget, type, value, ++action_sequence_, ticket);
    if (status) {
        std::fprintf(stderr, "HOME_WIDGET rejected: app=%u type=%u status=%d\n", widget.app, type, status);
        updated_at_ = 0;
        return;
    }
    widget_ticket_ = ticket;
    pending_type_ = type;
    pending_value_ = value;
    updated_at_ = 0;
    std::fprintf(stderr, "HOME_WIDGET pending: app=%u type=%u ticket=%u\n", widget.app, type, ticket);
}
void HomeApp::UpdateWidgets() {
    uint32_t first_media = UINT32_MAX, active_media = UINT32_MAX;
    for (uint32_t i = 0; i < 8; ++i) {
        widget_valid_[i] = sessions_ && sessions_->Widget(i, widgets_[i]);
        if (!widget_valid_[i]) continue;
        const auto& widget = widgets_[i];
        if (widget.revision != logged_revision_[i] || widget.online != logged_online_[i]) {
            std::fprintf(stderr, "HOME_WIDGET snapshot: app=%u template=%u revision=%u online=%d\n", widget.app,
                         widget.data.template_id, widget.revision, widget.online);
            logged_revision_[i] = widget.revision;
            logged_online_[i] = widget.online;
        }
        if (widget.data.template_id == MICROPIXEL_WIDGET_MEDIA) {
            if (first_media == UINT32_MAX) first_media = i;
            if (widget.app < apps_.size() && apps_[widget.app].get() == running_) active_media = i;
        }
    }
    media_widget_ = active_media < 8 ? active_media : first_media;
    unsigned visible = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const bool show = widget_valid_[i] && i != media_widget_;
        lv_obj_set_hidden(widget_cards_[i], !show);
        if (!show) continue;
        const int card_width = width_ < 600 ? width_ - 48 : 324;
        lv_obj_set_pos(widget_cards_[i], visible++ * (card_width + 12), 0);
        lv_obj_set_width(widget_cards_[i], card_width);
        const auto& widget = widgets_[i];
        lv_label_set_text(widget_titles_[i], widget.data.title);
        char subtitle[160]{};
        std::snprintf(subtitle, sizeof(subtitle), "%s%s",
                      !widget.online   ? "已停止 · "
                      : widget.expired ? "待更新 · "
                                       : "",
                      widget.data.subtitle);
        lv_label_set_text(widget_subtitles_[i], subtitle);
        lv_obj_set_hidden(widget_refresh_[i], widget.data.template_id != MICROPIXEL_WIDGET_WEATHER);
        lv_obj_set_state(widget_refresh_[i], LV_STATE_DISABLED, widget_ticket_ != 0);
    }
    const bool strip = visible && height_ >= 600;
    lv_obj_set_hidden(widget_strip_, !strip);
    lv_obj_set_height(catalog_, std::max(32, height_ - catalog_top_ - (strip ? 370 : height_ < 480 ? 54 : 182)));
    lv_obj_set_hidden(next_, media_widget_ >= 8 || width_ < 600);
    lv_obj_set_state(next_, LV_STATE_DISABLED,
                     widget_ticket_ || (media_widget_ < 8 && !widgets_[media_widget_].online));
}
void HomeApp::Launch(lv_event_t* event) {
    auto& home = *static_cast<HomeApp*>(lv_event_get_user_data(event));
    home.requested = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
}
void HomeApp::Action(lv_event_t* event) {
    auto& home = *static_cast<HomeApp*>(lv_event_get_user_data(event));
    switch (static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))))) {
        case kMusic:
            home.music_requested = true;
            break;
        case kMarket:
            home.market_requested = true;
            break;
        case kResume:
            if (home.media_widget_ < 8)
                home.requested = home.widgets_[home.media_widget_].app;
            else
                home.resume_requested = true;
            break;
        case kClose:
            if (home.media_widget_ < 8)
                home.stop_requested = home.widgets_[home.media_widget_].app;
            else
                home.close_requested = true;
            break;
        case kVolume:
            home.audio_.SetMasterVolume(lv_slider_get_value(lv_event_get_target_obj(event)));
            break;
        case kPause:
            if (home.media_widget_ < 8) {
                const auto& widget = home.widgets_[home.media_widget_];
                home.SendWidget(widget, MICROPIXEL_ACTION_SET_PLAYING,
                                !(widget.data.flags & MICROPIXEL_WIDGET_PLAYING));
                break;
            }
            home.media_->Pause(!home.media_->Paused());
            home.updated_at_ = 0;
            std::fprintf(stderr, "HOME_MEDIA paused=%d position_ms=%.0f\n", home.media_->Paused(),
                         home.media_->PositionSeconds() * 1000);
            break;
        case kNext:
            if (home.media_widget_ < 8)
                home.SendWidget(home.widgets_[home.media_widget_], MICROPIXEL_ACTION_NEXT_TRACK, 0);
            break;
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
    // System pages stay above every Guest frame, including frames published later.
    root_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root_, width, height);
    Panel(root_, 0x0b1423, 0);
    auto* title = Label(root_, 24, 25, width - 48, "MicroPixel", 0xf2f6fc);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    const bool compact = width < 600;
    const bool short_screen = height < 480;
    const int action_y = compact ? (short_screen ? 62 : 76) : 30;
    const int action_width = compact ? (width - 60) / 2 : 116;
    Button(root_, compact ? 24 : width - 268, action_y, action_width, short_screen ? 36 : 46, "网易云", kMusic, Action,
           this);
    Button(root_, width - 24 - (compact ? action_width : 120), action_y, compact ? action_width : 120,
           short_screen ? 36 : 46, "应用市场", kMarket, Action, this);
    Label(root_, 24, compact ? (short_screen ? 105 : 128) : 92, width - 48, "我的应用", 0x91a5c0);
    catalog_ = lv_obj_create(root_);
    const int top = compact ? (short_screen ? 134 : 164) : 130;
    catalog_top_ = top;
    lv_obj_set_pos(catalog_, 24, top);
    lv_obj_set_size(catalog_, width - 48, std::max(32, height - top - (short_screen ? 54 : 182)));
    Panel(catalog_, 0x0b1423, 0);
    lv_obj_set_scrollable(catalog_, true);
    lv_obj_set_scroll_dir(catalog_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(catalog_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(catalog_, 4, 0);
    const int columns = compact ? 1 : 2;
    const int card_width = (width - 48 - (columns - 1) * 12) / columns;
    constexpr std::array<uint32_t, 6> colors{0xe33b52, 0x437fd1, 0x238b7d, 0x9966ca, 0xc67b35, 0x52798d};
    for (size_t i = 0; i < apps_.size(); ++i) {
        auto* card = lv_button_create(catalog_);
        lv_obj_set_pos(card, (i % columns) * (card_width + 12), (i / columns) * 116);
        lv_obj_set_size(card, card_width, 104);
        Panel(card, 0x17253a);
        lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(card, Launch, LV_EVENT_SHORT_CLICKED, this);
        auto* icon = lv_obj_create(card);
        lv_obj_set_pos(icon, 16, 24);
        lv_obj_set_size(icon, 52, 52);
        Panel(icon, colors[i % colors.size()], 16);
        lv_obj_set_clickable(icon, false);
        const auto& name = apps_[i]->title;
        const auto lead = name.empty() ? 0U : static_cast<uint8_t>(name.front());
        const auto bytes = lead < 0x80 ? 1U : lead < 0xe0 ? 2U : lead < 0xf0 ? 3U : 4U;
        auto* initial = Label(icon, 0, 0, 44, name.empty() ? "A" : name.substr(0, bytes).c_str(), 0xffffff);
        lv_obj_set_style_text_align(initial, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(initial);
        Label(card, 84, 24, card_width - 96, name.c_str(), 0xf2f6fc);
        Label(card, 84, 57, card_width - 96, running_ == apps_[i].get() ? "正在运行 · 返回应用" : "点击打开", 0x91a5c0);
    }
    if (apps_.empty()) Label(catalog_, 8, 12, width - 80, "暂无应用，请先添加应用。", 0x91a5c0);
    lv_obj_update_layout(catalog_);
    lv_obj_scroll_to_y(catalog_, scroll_y_, LV_ANIM_OFF);
    widget_strip_ = lv_obj_create(root_);
    lv_obj_set_pos(widget_strip_, 24, height - 352);
    lv_obj_set_size(widget_strip_, width - 48, 168);
    Panel(widget_strip_, 0x0b1423, 0);
    lv_obj_set_scrollable(widget_strip_, true);
    lv_obj_set_scroll_dir(widget_strip_, LV_DIR_HOR);
    for (uint32_t i = 0; i < 8; ++i) {
        auto* card = lv_button_create(widget_strip_);
        widget_cards_[i] = card;
        lv_obj_set_size(card, 324, 156);
        Panel(card, 0x17253a);
        lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(card, WidgetEvent, LV_EVENT_SHORT_CLICKED, this);
        widget_titles_[i] = Label(card, 16, 16, 292, "", 0xf2f6fc);
        widget_subtitles_[i] = Label(card, 16, 56, 292, "", 0x91a5c0);
        widget_refresh_[i] = Button(card, 196, 104, 112, 36, "刷新", 100 + i, WidgetEvent, this);
    }
    auto* current = lv_obj_create(root_);
    lv_obj_set_pos(current, 24, height - 160);
    lv_obj_set_size(current, width - 48, 104);
    Panel(current, 0x17253a);
    lv_obj_set_hidden(current, short_screen);
    running_title_ = Label(current, 16, compact ? 8 : 14, compact ? width - 80 : width - 232, "", 0xf2f6fc);
    status_ = Label(current, 16, compact ? 34 : 48, compact ? width - 80 : width - 280, "", 0x91a5c0);
    pause_ = Button(current, compact ? 12 : width - 264, compact ? 66 : 55, compact ? 60 : 72, 32, "暂停", kPause,
                    Action, this);
    next_ = Button(current, width - 352, 55, 72, 32, "下一首", kNext, Action, this);
    resume_ = Button(current, compact ? width - 160 : width - 176, compact ? 66 : 14, compact ? 100 : 112,
                     compact ? 32 : 76, "返回应用", kResume, Action, this);
    close_ = Button(root_, width - 116, height - 44, 92, 32, "关闭应用", kClose, Action, this);
    Label(root_, 24, height - 42, width - (compact ? 154 : 360), "Home / F1 回主页 · Esc 退出应用", 0x91a5c0);
    if (!compact) {
        Label(root_, width - 318, height - 40, 48, "音量", 0x91a5c0);
        auto* volume = lv_slider_create(root_);
        lv_obj_set_pos(volume, width - 256, height - 32);
        lv_obj_set_size(volume, 116, 8);
        lv_slider_set_range(volume, 0, 100);
        lv_slider_set_value(volume, audio_.MasterVolume(), LV_ANIM_OFF);
        lv_obj_set_user_data(volume, reinterpret_cast<void*>(kVolume));
        lv_obj_add_event_cb(volume, Action, LV_EVENT_VALUE_CHANGED, this);
    }
    updated_at_ = 0;
    Update();
    std::fprintf(stderr, "HOME visible: running=%s apps=%zu\n", running_ ? running_->app_id.c_str() : "none",
                 apps_.size());
}
void HomeApp::Update() {
    if (!root_) return;
    if (widget_ticket_ && sessions_) {
        const auto result = sessions_->WidgetResult(widget_ticket_);
        if (result.complete) {
            std::fprintf(stderr, "HOME_WIDGET result: ticket=%u status=%d\n", widget_ticket_, result.result);
            if (result.result == 0 && pending_type_ == MICROPIXEL_ACTION_SET_PLAYING)
                std::fprintf(stderr, "HOME_MEDIA paused=%u position_ms=0\n", pending_value_ ? 0 : 1);
            widget_ticket_ = 0;
            updated_at_ = 0;
        }
    }
    const auto now = esp_timer_get_time();
    if (updated_at_ && now - updated_at_ < 250000) return;
    updated_at_ = now;
    UpdateWidgets();
    char status[160]{};
    if (media_widget_ < 8) {
        const auto& widget = widgets_[media_widget_];
        const bool playing = widget.data.flags & MICROPIXEL_WIDGET_PLAYING;
        lv_label_set_text(running_title_, widget.data.title);
        lv_obj_set_hidden(pause_, false);
        lv_obj_set_state(pause_, LV_STATE_DISABLED, !widget.online || widget_ticket_);
        lv_label_set_text(lv_obj_get_child(pause_, 0), playing ? "暂停" : "继续");
        const unsigned position = widget.data.position_ms / 1000, duration = widget.data.duration_ms / 1000;
        std::snprintf(status, sizeof(status), "%s  %02u:%02u / %02u:%02u",
                      !widget.online ? "应用已停止"
                      : playing      ? "正在播放"
                                     : "已暂停",
                      position / 60, position % 60, duration / 60, duration % 60);
        lv_obj_set_width(status_, width_ < 600 ? width_ - 80 : 220);
        lv_obj_set_state(resume_, LV_STATE_DISABLED, widget.app >= apps_.size());
        lv_obj_set_state(close_, LV_STATE_DISABLED, !widget.online);
        lv_label_set_text(lv_obj_get_child(close_, 0), "关闭音乐");
    } else {
        lv_label_set_text(running_title_, running_ ? running_->title.c_str() : "没有正在运行的应用");
        lv_obj_set_state(resume_, LV_STATE_DISABLED, !running_);
        lv_obj_set_state(close_, LV_STATE_DISABLED, !running_);
        lv_label_set_text(lv_obj_get_child(close_, 0), "关闭应用");
        const bool active = !media_->Finished() && !media_->Failed();
        lv_obj_set_hidden(pause_, !active);
        lv_obj_set_state(pause_, LV_STATE_DISABLED, false);
        lv_label_set_text(lv_obj_get_child(pause_, 0), media_->Paused() ? "继续" : "暂停");
        if (active) {
            const unsigned position = media_->PositionSeconds(), duration = media_->DurationSeconds();
            std::snprintf(status, sizeof(status), "%s  %02u:%02u / %02u:%02u", media_->Paused() ? "已暂停" : "正在播放",
                          position / 60, position % 60, duration / 60, duration % 60);
        } else
            std::snprintf(status, sizeof(status), "%s", running_ ? "应用在后台运行" : "打开一个应用开始使用");
    }
    lv_label_set_text(status_, status);
}
void HomeApp::Hide() {
    if (catalog_) scroll_y_ = lv_obj_get_scroll_y(catalog_);
    if (root_) lv_obj_delete(root_);
    root_ = catalog_ = running_title_ = status_ = resume_ = pause_ = close_ = widget_strip_ = next_ = nullptr;
    widget_cards_ = {};
    widget_titles_ = {};
    widget_subtitles_ = {};
    widget_refresh_ = {};
}
}  // namespace micropixel::pc
