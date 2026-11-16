// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "login.hpp"
#include "sdk/scene.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/text_button.hpp"
namespace netease {
enum class LoginAction { kNone, kBack, kRefresh, kRemember, kLogout };
class LoginView {
   public:
    void CancelInput() {
        back_.Reset();
        refresh_.Reset();
        remember_.Reset();
        logout_.Reset();
    }
    explicit LoginView(micropixel::Scene& scene) {
        using namespace micropixel;
        root_ = scene.CreateContainer({.clip = {0, 0, 720, 720}, .z_order = 100, .visible = false}).value();
        (void)root_.CreateShape({0, 0, 720, 720}, ui::panel::Background).value();
        heading_ = root_.CreateLabel({360, 22}, "微信扫码登录", ui::panel::Text, SystemFont::kLarge, true).value();
        hint_ =
            root_.CreateLabel({360, 76}, "使用微信扫一扫，登录网易云音乐", ui::panel::Muted, SystemFont::kMedium, true)
                .value();
        nickname_ = root_.CreateLabel({360, 310}, " ", ui::panel::Text, SystemFont::kLarge, true).value();
        status_ = root_.CreateLabel({360, 594}, " ", Color::Rgb(96, 217, 188), SystemFont::kSmall, true).value();
        auto button = [&](Rect bounds, const char* text) {
            return root_.CreateTextButton({.bounds = bounds,
                                           .text = text,
                                           .style = {.background = ui::panel::Surface,
                                                     .text = ui::panel::Accent,
                                                     .feedback = Color::White(),
                                                     .font = SystemFont::kMedium,
                                                     .corner_radius = 12},
                                           .overflow = ui::TextOverflow::kClip});
        };
        back_ = button({24, 16, 100, 44}, "返回");
        refresh_ = button({544, 16, 152, 44}, "刷新二维码");
        remember_ = button({24, 650, 282, 48}, "记住登录：关");
        logout_ = button({514, 650, 182, 48}, "退出登录");
        (void)root_.CreateLabel({24, 624}, "记住登录会将凭证保存在本机", ui::panel::Muted, SystemFont::kSmall).value();
    }
    void Show(bool show) { root_.SetVisible(show); }
    void Update(const Login& login, const Account& account) {
        heading_.SetText(account.verified() ? "网易云账号" : "微信扫码登录");
        remember_.SetText(account.remember() ? "记住登录：开" : "记住登录：关").value();
        logout_.SetVisible(account.verified());
        logout_.SetEnabled(account.verified());
        refresh_.SetText(account.verified() ? "切换账号" : "刷新二维码").value();
        nickname_.SetText(account.nickname.empty() ? " " : account.nickname.c_str());
        nickname_.SetVisible(account.verified());
        hint_.SetText(account.verified() ? "已通过网易云官网完成微信授权" : "使用微信扫一扫，登录网易云音乐");
        status_.SetText(login.message.empty() ? " " : login.message.c_str());
        if (login.qr.valid() && generation_ != login.qr_generation) {
            if (image_.valid()) image_.Destroy();
            image_ = root_
                         .CreateSprite(
                             login.qr, {125, 110, 470, 470},
                             {0, 0, static_cast<int32_t>(login.qr.width()), static_cast<int32_t>(login.qr.height())})
                         .value();
            generation_ = login.qr_generation;
        }
        if (image_.valid())
            image_.SetVisible(!account.verified() &&
                              (login.phase == LoginPhase::kWaiting || login.phase == LoginPhase::kScanned));
    }
    LoginAction Touch(const micropixel::TouchEvent& touch, bool& changed) {
        LoginAction action = LoginAction::kNone;
        auto check = [&](micropixel::ui::TextButton& button, LoginAction candidate) {
            auto update = button.OnTouch(touch);
            changed |= update.redraw();
            if (update.clicked) action = candidate;
        };
        check(back_, LoginAction::kBack);
        check(refresh_, LoginAction::kRefresh);
        check(remember_, LoginAction::kRemember);
        check(logout_, LoginAction::kLogout);
        return action;
    }

   private:
    micropixel::ContainerNode root_;
    micropixel::LabelNode heading_, hint_, nickname_, status_;
    micropixel::SpriteNode image_;
    micropixel::ui::TextButton back_, refresh_, remember_, logout_;
    uint32_t generation_{};
};
}  // namespace netease
