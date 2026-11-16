// SPDX-License-Identifier: Apache-2.0
#include "alarm_overlay.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
namespace micropixel::pc {
AlarmOverlay::~AlarmOverlay() {
    if (root_) lv_obj_delete(root_);
}
void AlarmOverlay::Action(lv_event_t* event) {
    auto& self = *static_cast<AlarmOverlay*>(lv_event_get_user_data(event));
    auto* target = lv_event_get_target_obj(event);
    if (target != self.pause_) {
        self.sessions_.StopAlarm();
        return;
    }
    if (self.ticket_) return;
    host::WidgetSnapshot widget{};
    for (unsigned i = 0; i < 8; ++i)
        if (self.sessions_.Widget(i, widget) && widget.online && widget.data.template_id == MICROPIXEL_WIDGET_MEDIA) {
            auto status = self.sessions_.WidgetAction(widget, MICROPIXEL_ACTION_SET_PLAYING, 0,
                                                      self.sessions_.NextHomeRequest(), self.ticket_);
            std::fprintf(stderr, "ALARM pause music: status=%d\n", status);
            break;
        }
}
void AlarmOverlay::Update(int width, int height) {
    auto state = sessions_.Alarm();
    if (!state.ringing) {
        if (root_) {
            lv_obj_delete(root_);
            root_ = label_ = pause_ = nullptr;
            lv_group_focus_freeze(lv_group_get_default(), false);
            std::fputs("ALARM overlay closed\n", stderr);
        }
        return;
    }
    if (!root_) {
        root_ = lv_obj_create(lv_layer_top());
        lv_obj_set_pos(root_, 0, 0);
        lv_obj_set_size(root_, 720, 720);
        int scale = std::min(width, height) * 256 / 720;
        lv_obj_set_style_transform_pivot_x(root_, 0, 0);
        lv_obj_set_style_transform_pivot_y(root_, 0, 0);
        lv_obj_set_style_transform_scale_x(root_, scale, 0);
        lv_obj_set_style_transform_scale_y(root_, scale, 0);
        lv_obj_set_pos(root_, (width - 720 * scale / 256) / 2, (height - 720 * scale / 256) / 2);
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x292017), 0);
        lv_obj_set_style_bg_grad_color(root_, lv_color_hex(0x101214), 0);
        lv_obj_set_style_bg_grad_dir(root_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(root_, 0, 0);
        lv_obj_set_style_radius(root_, 36, 0);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_set_scrollable(root_, false);
        lv_obj_set_style_text_color(root_, lv_color_hex(0xf5f7f8), 0);
        if (body_) lv_obj_set_style_text_font(root_, body_, 0);
        auto* title = lv_label_create(root_);
        lv_label_set_text(title, "闹钟提醒");
        lv_obj_set_style_text_color(title, lv_color_hex(0xe8a15c), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 132);
        time_ = lv_label_create(root_);
        lv_obj_set_width(time_, 672);
        lv_obj_set_style_text_align(time_, LV_TEXT_ALIGN_CENTER, 0);
        if (numerals_) lv_obj_set_style_text_font(time_, numerals_, 0);
        lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, 216);
        label_ = lv_label_create(root_);
        lv_obj_set_width(label_, 640);
        lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(label_, lv_color_hex(0x8c979e), 0);
        lv_obj_align(label_, LV_ALIGN_TOP_MID, 0, 430);
        for (int i = 0; i < 2; ++i) {
            auto* button = lv_button_create(root_);
            lv_obj_set_size(button, 324, 96);
            lv_obj_set_pos(button, i ? 372 : 24, 582);
            lv_obj_set_style_radius(button, 30, 0);
            lv_obj_set_style_border_width(button, 0, 0);
            lv_obj_set_style_shadow_width(button, 0, 0);
            lv_obj_set_style_bg_color(button, lv_color_hex(i ? 0x242a2f : 0xe8a15c), 0);
            lv_obj_set_style_text_color(button, lv_color_hex(i ? 0xf5f7f8 : 0x1a1305), 0);
            auto* label = lv_label_create(button);
            lv_label_set_text(label, i ? "暂停音乐" : "停止响铃");
            lv_obj_center(label);
            lv_obj_add_event_cb(button, Action, LV_EVENT_SHORT_CLICKED, this);
            if (i)
                pause_ = button;
            else {
                lv_group_focus_obj(button);
                lv_group_focus_freeze(lv_group_get_default(), true);
            }
        }
        std::fputs("ALARM overlay visible\n", stderr);
    }
    auto wall = sessions_.Clock();
    auto seconds = static_cast<time_t>(wall.utc + int64_t(wall.offset_minutes) * 60);
    std::tm local{};
    gmtime_r(&seconds, &local);
    char clock[16]{};
    std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
    if (std::strcmp(lv_label_get_text(time_), clock)) lv_label_set_text(time_, clock);
    if (state.revision != revision_) {
        char text[160]{};
        std::snprintf(text, sizeof(text), "%s\n%u 个提醒 · 到时间了", state.label[0] ? state.label.data() : "到时间了",
                      state.count);
        lv_label_set_text(label_, text);
        revision_ = state.revision;
    }
    if (ticket_ && sessions_.WidgetResult(ticket_).complete) ticket_ = 0;
    lv_obj_set_state(pause_, LV_STATE_DISABLED, ticket_ != 0);
    lv_obj_move_foreground(root_);
}
}  // namespace micropixel::pc
