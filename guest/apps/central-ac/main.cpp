// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <string_view>

#include "central-ac_assets.hpp"
#include "panel_view.hpp"

namespace central_ac {
namespace {
Preview InitialPreview(const mp::LaunchArguments& arguments) {
    const char* value = arguments.FindValue("--preview");
    if (value == nullptr) return Preview::kNormal;
    const std::string_view name(value);
    if (name == "local") return Preview::kLocal;
    if (name == "offline") return Preview::kOffline;
    if (name == "restricted") return Preview::kRestricted;
    if (name == "pending") return Preview::kPending;
    if (name == "timeout") return Preview::kTimeout;
    return Preview::kNormal;
}

class PanelActivity final : public mp::Activity {
   private:
    struct View {
        explicit View(const mp::Application& app)
            : numbers(app, colors::Text),
              icons(app.resources().LoadTexture(central_ac_assets::climate_icons, mp::TextureScale::kNative).value()),
              scene(app.renderer().CreateScene(colors::Background).value()),
              home(scene, numbers, icons),
              picker(scene) {}

        ui::NumeralAtlas numbers;
        mp::Texture icons;
        mp::Scene scene;
        HomeView home;
        PickerView picker;
    };

    void OnCreate(mp::ComponentContext& context) override {
        const auto& app = context.app();
        state_ = {.preview = InitialPreview(app.launch_arguments())};
        page_ = Page::kHome;
        view_ = std::make_unique<View>(app);
        view_->home.Show(state_);
        Present(app);
    }

    void OnResume(mp::ComponentContext& context) override {
        visible_ = true;
        Present(context.app());
    }

    void OnStop(mp::ComponentContext&) override {
        visible_ = false;
        view_->home.Reset();
        view_->picker.Reset();
    }

    void OnDestroy(mp::ComponentContext&) override {
        visible_ = false;
        view_.reset();
    }

    bool OnNavigateBack(mp::ComponentContext& context) override {
        if (page_ == Page::kHome) return false;
        ShowHome();
        Present(context.app());
        return true;
    }

    void OnEvent(mp::ComponentContext& context, const mp::Event& event) override {
        const auto* touch = event.touch();
        if (!visible_ || touch == nullptr) return;
        const auto update = page_ == Page::kHome ? view_->home.Touch(*touch) : view_->picker.Touch(*touch);
        if (update.action != Action::kNone) Apply(update);
        if (update.redraw) Present(context.app());
    }

    void Apply(TouchResult update) {
        switch (update.action) {
            case Action::kLower:
                --state_.temperature;
                break;
            case Action::kRaise:
                ++state_.temperature;
                break;
            case Action::kPower:
                state_.powered = !state_.powered;
                if (!state_.powered) state_.timer = kNoSelection;
                break;
            case Action::kMode:
                return ShowPicker(Page::kMode);
            case Action::kFan:
                return ShowPicker(Page::kFan);
            case Action::kTimer:
                draft_ = state_.timer;
                return ShowPicker(Page::kTimer);
            case Action::kChoose:
                if (page_ == Page::kTimer) {
                    draft_ = update.option;
                    return ShowPicker(Page::kTimer);
                }
                if (page_ == Page::kMode) state_.mode = update.option;
                if (page_ == Page::kFan) state_.fan = update.option;
                break;
            case Action::kSave:
                state_.timer = draft_;
                break;
            case Action::kClearTimer:
                state_.timer = kNoSelection;
                break;
            case Action::kRefresh:
                state_.preview = Preview::kNormal;
                break;
            case Action::kBack:
            case Action::kNone:
                break;
        }
        ShowHome();
    }

    void ShowPicker(Page page) {
        page_ = page;
        view_->home.Reset();
        view_->home.SetVisible(false);
        view_->picker.Show(page_, state_, draft_);
    }

    void ShowHome() {
        page_ = Page::kHome;
        view_->picker.Hide();
        view_->home.Show(state_);
        view_->home.SetVisible(true);
    }

    void Present(const mp::Application& app) { app.renderer().Present(view_->scene).value(); }

    PanelState state_;
    Page page_{Page::kHome};
    size_t draft_{kNoSelection};
    bool visible_{};
    std::unique_ptr<View> view_;
};
}  // namespace
}  // namespace central_ac

int main() {
    micropixel::Application app;
    app.renderer()
        .ConfigureDisplay({.logical_size = {central_ac::kCanvas, central_ac::kCanvas},
                           .scale_mode = micropixel::DisplayScaleMode::kAspectFit})
        .value();
    central_ac::PanelActivity activity;
    app.RunComponents(&activity, nullptr);
    return 0;
}
