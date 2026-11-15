// SPDX-License-Identifier: Apache-2.0
#include <optional>

#include "sdk/micropixel.hpp"
namespace clock_app {
using namespace micropixel;
struct Model {
    unsigned hour{8}, minute{};
    bool daily{true}, enabled{};
    int32_t offset{480};
    uint32_t revision{};
};
FixedString<64> TimeText(uint64_t utc, int32_t offset, bool seconds) {
    FixedString<64> out;
    auto n = (static_cast<int64_t>(utc) + int64_t(offset) * 60) % 86400;
    if (n / 3600 < 10) out.Append("0");
    out.AppendInt(n / 3600);
    out.Append(":");
    if ((n / 60) % 60 < 10) out.Append("0");
    out.AppendInt((n / 60) % 60);
    if (seconds) {
        out.Append(":");
        if (n % 60 < 10) out.Append("0");
        out.AppendInt(n % 60);
    }
    return out;
}
class ClockService final : public AppService {
    void OnStart(ComponentContext& context) override {
        const auto& app = context.app();
        (void)app.widgets().Publish(1, {.kind = WidgetTemplate::kClock,
                                        .title = "时钟闹钟",
                                        .subtitle = "Host 计时与闹钟",
                                        .valid_for_seconds = 0});
        app.log().Info("CLOCK service ready");
    }
};
class ClockActivity final : public Activity {
   public:
    explicit ClockActivity(Model& model) : model_(model) {}
    void OnCreate(ComponentContext& context) override {
        const auto& app = context.app();
        auto now = app.alarms().Now().value();
        model_.offset = now.offset_minutes;
        auto saved = app.alarms().Get(1);
        if (saved) {
            model_.enabled = saved->enabled;
            model_.daily = saved->weekly;
            if (saved->weekly) {
                model_.hour = saved->minute_of_day / 60;
                model_.minute = saved->minute_of_day % 60;
            } else {
                auto local = (static_cast<int64_t>(saved->once_utc) + int64_t(now.offset_minutes) * 60) % 86400;
                model_.hour = local / 3600;
                model_.minute = (local / 60) % 60;
            }
        }
        scene_.emplace(app.renderer().CreateScene(Color::Rgb(241, 245, 251)).value());
        (void)scene_->CreateLabel({32, 36}, "时钟与闹钟", Color::Black(), SystemFont::kTitle).value();
        time_ = scene_->CreateLabel({32, 112}, "", Color::Black(), SystemFont::kTitle).value();
        alarm_ = scene_->CreateLabel({32, 200}, "", Color::Black(), SystemFont::kLarge).value();
        status_ = scene_->CreateLabel({32, 600}, "设置保存在 Host", Color::Black(), SystemFont::kSmall).value();
        hour_ = scene_->CreateTextButton({.bounds = {32, 285, 310, 60}, .text = "小时 +1"});
        minute_ = scene_->CreateTextButton({.bounds = {370, 285, 310, 60}, .text = "分钟 +1"});
        repeat_ = scene_->CreateTextButton({.bounds = {32, 365, 310, 60}, .text = "每天重复"});
        save_ = scene_->CreateTextButton({.bounds = {370, 365, 310, 60}, .text = "保存并启用"});
        disable_ = scene_->CreateTextButton({.bounds = {32, 445, 310, 60}, .text = "停用闹钟"});
        test_ = scene_->CreateTextButton({.bounds = {370, 445, 310, 60}, .text = "10 秒后提醒"});
        zone_ = scene_->CreateTextButton({.bounds = {32, 525, 310, 54}, .text = "切换时区"});
        sync_ = scene_->CreateTextButton({.bounds = {370, 525, 310, 54}, .text = "同步系统时间"});
        (void)scene_
            ->CreateLabel({32, 650}, "关闭应用后闹钟继续；停用或清除数据才取消", Color::Black(), SystemFont::kSmall)
            .value();
        timer_.emplace(app.timers().Every(Duration::Seconds(1)).value());
        Draw(app);
        app.log().Info("CLOCK activity created");
        if (app.launch_arguments().HasFlag("--alarm-test")) {
            auto plan = Plan(now);
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
    void OnStop(ComponentContext&) override {
        hour_.Reset();
        minute_.Reset();
        repeat_.Reset();
        save_.Reset();
        disable_.Reset();
        test_.Reset();
        zone_.Reset();
        sync_.Reset();
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        scene_.reset();
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if (timer_ && event.TimerFrom(*timer_)) {
            Draw(app);
            return;
        }
        auto* touch = event.touch();
        if (!touch) return;
        if (hour_.OnTouch(*touch).clicked) model_.hour = (model_.hour + 1) % 24;
        if (minute_.OnTouch(*touch).clicked) model_.minute = (model_.minute + 1) % 60;
        if (repeat_.OnTouch(*touch).clicked) model_.daily = !model_.daily;
        if (save_.OnTouch(*touch).clicked) {
            auto now = app.alarms().Now().value();
            auto saved = app.alarms().Register(Plan(now));
            model_.enabled = saved.has_value();
            status_.SetText(saved ? "闹钟已持久保存" : "保存失败或时间未同步");
            app.log().Info(saved ? "CLOCK saved" : "CLOCK save failed");
        }
        if (disable_.OnTouch(*touch).clicked) {
            auto saved = app.alarms().Get(1);
            if (saved) {
                saved->enabled = false;
                auto result = app.alarms().Register(*saved);
                if (result) {
                    model_.enabled = false;
                    status_.SetText("闹钟已停用");
                }
            }
            (void)app.alarms().Remove(2);
        }
        if (test_.OnTouch(*touch).clicked) {
            auto now = app.alarms().Now().value();
            auto plan = Plan(now);
            plan.id = 2;
            plan.weekly = false;
            plan.weekdays = 0;
            plan.minute_of_day = 0;
            plan.once_utc = now.utc + 10;
            auto saved = app.alarms().Register(plan);
            status_.SetText(saved ? "10 秒后由 Host 提醒" : "提醒保存失败");
            app.log().Info(saved ? "CLOCK test alarm registered" : "CLOCK save failed");
        }
        if (zone_.OnTouch(*touch).clicked) {
            model_.offset = model_.offset == 480 ? 0 : 480;
            auto result = app.alarms().ConfigureTime(true, model_.offset, app.alarms().Now().value().utc);
            status_.SetText(result ? "时区已保存，重算未来闹钟" : "时区保存失败");
        }
        if (sync_.OnTouch(*touch).clicked) {
            auto result = app.alarms().ConfigureTime(true, model_.offset);
            status_.SetText(result ? "已同步系统时间" : "同步失败");
        }
        Draw(app);
    }

   private:
    AlarmPlan Plan(const WallTime& now) {
        AlarmPlan plan;
        plan.enabled = true;
        plan.weekly = model_.daily;
        plan.label.Append("闹钟提醒");
        if (plan.weekly) {
            plan.weekdays = 127;
            plan.minute_of_day = model_.hour * 60 + model_.minute;
        } else {
            auto local = static_cast<int64_t>(now.utc) + int64_t(now.offset_minutes) * 60;
            auto target =
                (local / 86400) * 86400 + model_.hour * 3600 + model_.minute * 60 - int64_t(now.offset_minutes) * 60;
            if (target <= static_cast<int64_t>(now.utc)) target += 86400;
            plan.once_utc = target;
        }
        return plan;
    }
    void Draw(const Application& app) {
        if (!scene_) return;
        auto now = app.alarms().Now().value();
        auto registered = app.alarms().Get(1);
        model_.enabled = registered && registered->enabled;
        auto text = TimeText(now.utc, now.offset_minutes, true);
        time_.SetText(now.valid ? text.c_str() : "时间未同步");
        FixedString<96> label;
        label.Append(model_.enabled ? "已开启  " : "设置闹钟  ");
        if (model_.hour < 10) label.Append("0");
        label.AppendUint(model_.hour);
        label.Append(":");
        if (model_.minute < 10) label.Append("0");
        label.AppendUint(model_.minute);
        alarm_.SetText(label.c_str());
        repeat_.SetText(model_.daily ? "每天重复" : "仅一次").value();
        app.renderer().Present(*scene_).value();
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::optional<Timer> timer_;
    LabelNode time_, alarm_, status_;
    ui::TextButton hour_, minute_, repeat_, save_, disable_, test_, zone_, sync_;
};
}  // namespace clock_app
int main() {
    micropixel::Application app;
    clock_app::Model model;
    clock_app::ClockActivity page(model);
    clock_app::ClockService service;
    app.RunComponents(&page, &service);
    return 0;
}
