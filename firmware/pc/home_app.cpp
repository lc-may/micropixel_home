// SPDX-License-Identifier: Apache-2.0
#include "home_app.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"

namespace micropixel::pc {
namespace {
enum ActionId { kMusic, kMarket, kResume, kPause, kClose, kVolume };
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
HomeApp::HomeApp(std::vector<std::unique_ptr<PortableApp>>& apps, MediaPlayer& media, SdlAudio& audio)
    : apps_(apps), media_(&media), audio_(audio) {}
HomeApp::~HomeApp() { Hide(); }
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
            home.resume_requested = true;
            break;
        case kClose:
            home.close_requested = true;
            break;
        case kVolume:
            home.audio_.SetMasterVolume(lv_slider_get_value(lv_event_get_target_obj(event)));
            break;
        case kPause:
            home.media_->Pause(!home.media_->Paused());
            home.updated_at_ = 0;
            std::fprintf(stderr, "HOME_MEDIA paused=%d position_ms=%.0f\n", home.media_->Paused(),
                         home.media_->PositionSeconds() * 1000);
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
    auto* current = lv_obj_create(root_);
    lv_obj_set_pos(current, 24, height - 160);
    lv_obj_set_size(current, width - 48, 104);
    Panel(current, 0x17253a);
    lv_obj_set_hidden(current, short_screen);
    running_title_ = Label(current, 16, compact ? 8 : 14, compact ? width - 80 : width - 232, "", 0xf2f6fc);
    status_ = Label(current, 16, compact ? 34 : 48, compact ? width - 80 : width - 280, "", 0x91a5c0);
    pause_ = Button(current, compact ? 12 : width - 264, compact ? 66 : 55, compact ? 60 : 72, 32, "暂停", kPause,
                    Action, this);
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
    const auto now = esp_timer_get_time();
    if (updated_at_ && now - updated_at_ < 250000) return;
    updated_at_ = now;
    lv_label_set_text(running_title_, running_ ? running_->title.c_str() : "没有正在运行的应用");
    lv_obj_set_state(resume_, LV_STATE_DISABLED, !running_);
    lv_obj_set_state(close_, LV_STATE_DISABLED, !running_);
    const bool active = !media_->Finished() && !media_->Failed();
    lv_obj_set_hidden(pause_, !active);
    auto* pause_label = lv_obj_get_child(pause_, 0);
    lv_label_set_text(pause_label, media_->Paused() ? "继续" : "暂停");
    char status[96]{};
    if (active) {
        const auto position = static_cast<unsigned>(media_->PositionSeconds());
        const auto duration = static_cast<unsigned>(media_->DurationSeconds());
        std::snprintf(status, sizeof(status), "%s  %02u:%02u / %02u:%02u", media_->Paused() ? "已暂停" : "正在播放",
                      position / 60, position % 60, duration / 60, duration % 60);
    } else
        std::snprintf(status, sizeof(status), "%s", running_ ? "应用在后台运行" : "打开一个应用开始使用");
    lv_label_set_text(status_, status);
}
void HomeApp::Hide() {
    if (catalog_) scroll_y_ = lv_obj_get_scroll_y(catalog_);
    if (root_) lv_obj_delete(root_);
    root_ = catalog_ = running_title_ = status_ = resume_ = pause_ = close_ = nullptr;
}
}  // namespace micropixel::pc
