// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>

#include "clock_page.hpp"
namespace clock_app {
class ClockActivity final : public Activity {
   public:
    explicit ClockActivity(Model& model) : model_(model) {}

   private:
    void OnCreate(ComponentContext& c) override {
        const auto& app = c.app();
        model_.Read(app);
        scene_.emplace(app.renderer().CreateScene(Background).value());
        font_ = std::make_unique<ui::NumeralAtlas>(app, Text, 96);
        icons_ = std::make_unique<ui::MaterialAtlas>(app);
        const ui::AppBarAction actions[] = {{.icon = ui::MaterialSymbol::Add}};
        header_ = ui::AppBar::Create(*scene_,
                                     {.title = "闹钟",
                                      .leading = ui::AppBarAction{.icon = ui::MaterialSymbol::Back},
                                      .actions = actions,
                                      .subtitle_width = 110},
                                     icons_.get())
                      .value();
        timer_.emplace(app.timers().Every(Duration::Milliseconds(250)).value());
        Build(app);
        Draw(app);
        app.log().Info("CLOCK activity created");
        if (app.launch_arguments().HasFlag("--alarm-test")) {
            auto now = app.alarms().Now().value();
            auto plan = model_.Plan(now);
            plan.id = 2;
            plan.weekly = false;
            plan.weekdays = 0;
            plan.minute_of_day = 0;
            plan.once_utc = now.utc + 5;
            app.alarms().Register(plan).value();
            app.log().Info("CLOCK test alarm registered");
        }
    }
    void OnResume(ComponentContext& c) override { Draw(c.app()); }
    void OnStop(ComponentContext& c) override {
        header_.CancelInput();
        if (view_) {
            view_->Reset();
            view_->StopInput(c.app());
        }
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        view_.reset();
        header_.Destroy();
        scene_.reset();
        font_.reset();
        icons_.reset();
    }
    void OnEvent(ComponentContext& c, const Event& event) override {
        const auto& app = c.app();
        if (page_ != model_.page) Build(app);
        bool rebuild = false;
        if (auto* touch = event.touch()) {
            bool header = false;
            auto action = header_.OnTouch(*touch);
            if (action.action && action.action->kind == ui::AppBarActionKind::kLeading && model_.page != Alarm) {
                model_.page = model_.page == Editor ? Notes : Alarm;
                rebuild = header = true;
            }
            if ((model_.page == Alarm || model_.page == Calendar || model_.page == Notes) && action.action &&
                action.action->kind == ui::AppBarActionKind::kAction) {
                if (model_.page == Alarm) {
                    model_.page = AlarmEditor;
                    rebuild = true;
                } else
                    rebuild = model_.New(app);
                header = true;
            }
            if (!header && !action.handled && view_) rebuild |= view_->Touch(app, *touch);
        }
        if (view_ && view_->TextInput(app, event)) ++model_.revision;
        if (rebuild) Build(app);
        if (timer_ && event.TimerFrom(*timer_)) {
            auto before = model_.revision;
            model_.Read(app);
            if (page_ != model_.page || ((page_ == Alarm || page_ == Notes) && before != model_.revision)) Build(app);
        }
        Draw(app);
    }
    void Build(const Application& app) {
        if (view_) view_->StopInput(app);
        view_.reset();
        page_ = model_.page;
        view_ = std::make_unique<PageView>(*scene_, model_, *font_, *icons_);
        app.log().Info(page_ == Calendar      ? "CLOCK calendar opened"
                       : page_ == Notes       ? "CLOCK notes opened"
                       : page_ == Editor      ? "CLOCK editor opened"
                       : page_ == AlarmEditor ? "CLOCK alarm editor opened"
                                              : "CLOCK alarm opened");
    }
    void Draw(const Application& app) {
        auto now = app.alarms().Now().value();
        header_.SetSubtitle(now.valid ? TimeText(now.utc, now.offset_minutes, false).c_str() : "--:--").value();
        const char* titles[]{"闹钟", "日历", "事项", "编辑提醒", "设置闹钟"};
        header_.SetTitle(titles[model_.page]).value();
        header_.SetLeadingVisible(model_.page != Alarm);
        header_.SetActionVisible(0, model_.page <= Notes);
        if (view_) view_->Draw(app);
        app.renderer().Present(*scene_).value();
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::unique_ptr<ui::NumeralAtlas> font_;
    std::unique_ptr<ui::MaterialAtlas> icons_;
    std::optional<Timer> timer_;
    ui::AppBar header_;
    std::unique_ptr<PageView> view_;
    unsigned page_{UINT32_MAX};
};
}  // namespace clock_app
