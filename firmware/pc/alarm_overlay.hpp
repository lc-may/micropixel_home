// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "app_sessions.hpp"
namespace micropixel::pc {
class AlarmOverlay final {
   public:
    explicit AlarmOverlay(AppSessions& sessions, const lv_font_t* numerals = nullptr, const lv_font_t* body = nullptr)
        : sessions_(sessions), numerals_(numerals), body_(body) {}
    ~AlarmOverlay();
    void Update(int width, int height);
    bool Visible() const { return root_ != nullptr; }

   private:
    static void Action(lv_event_t*);
    AppSessions& sessions_;
    lv_obj_t *root_{}, *label_{}, *pause_{}, *time_{};
    const lv_font_t *numerals_{}, *body_{};
    uint64_t revision_{};
    uint32_t ticket_{};
};
}  // namespace micropixel::pc
