// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "planner.hpp"
#include "sdk/ui/panel.hpp"
namespace clock_app {
using namespace ui::panel;
class ClockService final : public AppService {
   public:
    explicit ClockService(Model& model) : model_(model) {}

   private:
    void OnStart(ComponentContext& context) override {
        (void)context.app().widgets().Publish(
            1, {.kind = WidgetTemplate::kClock, .title = "日历与提醒", .subtitle = "时钟、闹钟与日程"});
        context.app().log().Info("CLOCK service ready");
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        if (auto* incoming = event.message()) {
            auto message = context.app().messages().Read(incoming->ticket);
            int status = -2;
            if (message && message->type == static_cast<uint16_t>(AppAction::kOpenPage)) {
                auto page = message->U32();
                if (page && *page <= Notes) {
                    model_.page = *page;
                    ++model_.revision;
                    status = 0;
                    context.app().log().Info("CLOCK page requested");
                }
            }
            (void)context.app().messages().Ack(incoming->ticket, status);
        }
    }
    Model& model_;
};
class PageView {
   public:
    PageView(Scene& scene, Model& model) : model_(model) {
        root_ = scene.CreateContainer().value();
        Build();
    }
    ~PageView() {
        if (root_.valid()) root_.Destroy().value();
    }
    void Reset() {
        for (auto& b : buttons_)
            if (b.valid()) b.Reset();
        for (auto& b : days_)
            if (b.valid()) b.Reset();
        for (auto& b : done_)
            if (b.valid()) b.Reset();
        for (auto& b : remove_)
            if (b.valid()) b.Reset();
        for (auto& b : edit_) b.Reset();
    }
    bool Touch(const Application& app, const TouchEvent& touch) {
        bool rebuild = false;
        if (model_.page == Alarm) {
            if (Hit(0, touch)) model_.hour = (model_.hour + 1) % 24;
            if (Hit(1, touch)) model_.minute = (model_.minute + 1) % 60;
            if (Hit(2, touch)) model_.daily = !model_.daily;
            if (Hit(3, touch)) {
                auto result = app.alarms().Register(model_.Plan(app.alarms().Now().value()));
                model_.title_status = result ? "闹钟已保存" : "保存失败，请检查时间";
                app.log().Info(result ? "CLOCK saved" : "CLOCK save failed");
            }
            if (Hit(4, touch)) {
                auto plan = app.alarms().Get(1);
                if (plan) {
                    plan->enabled = false;
                    (void)app.alarms().Register(*plan);
                }
                (void)app.alarms().Remove(2);
                model_.title_status = "闹钟已停用";
            }
            if (Hit(5, touch)) {
                auto now = app.alarms().Now().value();
                auto plan = model_.Plan(now);
                plan.id = 2;
                plan.weekly = false;
                plan.weekdays = 0;
                plan.minute_of_day = 0;
                plan.once_utc = now.utc + 10;
                auto result = app.alarms().Register(plan);
                model_.title_status = result ? "10 秒后提醒" : "提醒保存失败";
                app.log().Info(result ? "CLOCK test alarm registered" : "CLOCK save failed");
            }
            if (Hit(6, touch)) {
                model_.offset = model_.offset == 480 ? 0 : 480;
                (void)app.alarms().ConfigureTime(true, model_.offset, app.alarms().Now().value().utc);
            }
            if (Hit(7, touch)) {
                (void)app.alarms().ConfigureTime(true, model_.offset);
                model_.title_status = "已同步系统时间";
            }
        } else if (model_.page == Calendar) {
            if (Hit(0, touch) && (model_.year > 1970 || model_.month > 1)) {
                if (--model_.month == 0) {
                    model_.month = 12;
                    --model_.year;
                }
                rebuild = true;
            }
            if (Hit(1, touch) && (model_.year < 2099 || model_.month < 12)) {
                if (++model_.month == 13) {
                    model_.month = 1;
                    ++model_.year;
                }
                rebuild = true;
            }
            if (Hit(2, touch)) {
                model_.selected = calendar::LocalDate(app.alarms().Now().value().utc, model_.offset);
                model_.year = model_.selected.year;
                model_.month = model_.selected.month;
                rebuild = true;
            }
            if (Hit(3, touch)) {
                rebuild = model_.New(app);
            }
            for (unsigned i = 0; i < 42; ++i)
                if (days_[i].valid() && days_[i].OnTouch(touch).clicked) {
                    model_.selected = {model_.year, model_.month, day_values_[i]};
                    rebuild = true;
                }
        } else if (model_.page == Notes) {
            for (unsigned i = 0; i < 6; ++i) {
                if (done_[i].valid() && done_[i].OnTouch(touch).clicked) {
                    model_.Toggle(app, i);
                    rebuild = true;
                }
                if (remove_[i].valid() && remove_[i].OnTouch(touch).clicked) {
                    model_.Delete(app, i);
                    rebuild = true;
                }
                if (edit_[i].OnTouch(touch).clicked) {
                    model_.Edit(i);
                    rebuild = true;
                }
            }
            if (Hit(0, touch)) rebuild = model_.New(app);
        } else {
            if (Hit(0, touch)) {
                typing_ = true;
                app.input().EnableText(true).value();
                model_.title_status = "使用键盘或输入法输入，回车收起";
            }
            if (Hit(1, touch)) {
                model_.draft_date = calendar::FromDays(std::max<int64_t>(0, calendar::Days(model_.draft_date) - 1));
            }
            if (Hit(2, touch)) {
                model_.draft_date = calendar::FromDays(calendar::Days(model_.draft_date) + 1);
            }
            if (Hit(3, touch)) model_.draft_minute = (model_.draft_minute + 1380) % 1440;
            if (Hit(4, touch)) model_.draft_minute = (model_.draft_minute + 60) % 1440;
            if (Hit(5, touch)) model_.draft_minute = (model_.draft_minute + 1435) % 1440;
            if (Hit(6, touch)) model_.draft_minute = (model_.draft_minute + 5) % 1440;
            if (Hit(7, touch)) {
                StopInput(app);
                rebuild = model_.Save(app);
            }
            if (Hit(8, touch)) {
                StopInput(app);
                model_.page = Notes;
                rebuild = true;
            }
        }
        ++model_.revision;
        return rebuild;
    }
    bool TextInput(const Application& app, const Event& event) {
        if (!typing_) return false;
        auto text = app.input().ReadText();
        if (text && !text->empty()) {
            if (model_.draft.size() + text->size() <= 63)
                model_.draft += text->c_str();
            else
                model_.title_status = "名称过长，请缩短";
        }
        if (auto* key = event.key(); key && key->phase() == KeyPhase::kDown) {
            if (key->code() == KeyCode::kConfirm) {
                StopInput(app);
                return true;
            }
            if (key->code() == KeyCode::kBack && !model_.draft.empty()) {
                auto n = model_.draft.size() - 1;
                while (n && (uint8_t(model_.draft[n]) & 0xc0) == 0x80) --n;
                model_.draft.resize(n);
            }
        }
        return true;
    }
    void StopInput(const Application& app) {
        if (typing_) {
            (void)app.input().EnableText(false);
            typing_ = false;
        }
    }
    void Draw(const Application& app) {
        auto now = app.alarms().Now().value();
        if (model_.page == Alarm) {
            auto saved = app.alarms().Get(1);
            model_.enabled = saved && saved->enabled;
            auto time = calendar::FormatTime(model_.hour * 60 + model_.minute);
            digits_.Set(time.c_str());
            label_.SetText(model_.enabled ? "闹钟已开启" : "闹钟未开启");
            buttons_[2].SetText(model_.daily ? "每天重复" : "仅一次").value();
        } else if (model_.page == Calendar) {
            unsigned count = 0;
            for (auto& note : model_.notes)
                if (note && calendar::LocalDate(note->once_utc, model_.offset) == model_.selected) ++count;
            FixedString<96> text;
            auto date = calendar::FormatDate(model_.selected);
            text.Append(date.c_str());
            text.Append(" · ");
            text.AppendUint(count);
            text.Append(" 项日程");
            label_.SetText(text.c_str());
        } else if (model_.page == Editor) {
            buttons_[0].SetText(model_.draft.empty() ? "点击输入事项名称" : model_.draft.c_str()).value();
            auto date = calendar::FormatDate(model_.draft_date);
            label_.SetText(date.c_str());
            auto time = calendar::FormatTime(model_.draft_minute);
            digits_.Set(time.c_str());
        }
        status_.SetText(model_.title_status.empty() ? (model_.page == Editor ? "到点提醒，关闭应用后仍有效" : " ")
                                                    : model_.title_status.c_str());
        (void)now;
    }

   private:
    bool Hit(unsigned index, const TouchEvent& event) {
        return buttons_[index].valid() && buttons_[index].OnTouch(event).clicked;
    }
    void Button(unsigned i, Rect r, const char* text, bool active = false) {
        buttons_[i] = root_.CreateTextButton({.bounds = r, .text = text, .style = Style(active)});
    }
    void Build() {
        if (model_.page == Alarm) {
            Card(root_, {24, 164, 672, 100});
            digits_ = Digits::Create(root_, {50, 184}, 60, Text);
            label_ = root_.CreateLabel({310, 200}, " ", Muted, SystemFont::kLarge).value();
            Button(0, {32, 285, 310, 60}, "小时 +1");
            Button(1, {370, 285, 310, 60}, "分钟 +1");
            Button(2, {32, 365, 310, 60}, "每天重复");
            Button(3, {370, 365, 310, 60}, "保存并启用", true);
            Button(4, {32, 445, 310, 60}, "停用闹钟");
            Button(5, {370, 445, 310, 60}, "10 秒后提醒");
            Button(6, {32, 525, 310, 54}, "切换时区");
            Button(7, {370, 525, 310, 54}, "同步系统时间");
            (void)root_.CreateLabel({32, 620}, "为明天留一个温柔的提醒", Muted, SystemFont::kMedium).value();
        } else if (model_.page == Calendar) {
            FixedString<32> month;
            month.AppendInt(model_.year);
            month.Append(" 年 ");
            month.AppendUint(model_.month);
            month.Append(" 月");
            (void)root_.CreateLabel({360, 176}, month.c_str(), Text, SystemFont::kTitle, true).value();
            Button(0, {24, 164, 64, 56}, symbols::kLeft);
            Button(1, {632, 164, 64, 56}, symbols::kRight);
            constexpr const char* week[]{"一", "二", "三", "四", "五", "六", "日"};
            for (unsigned i = 0; i < 7; ++i)
                (void)root_.CreateLabel({72 + int(i) * 96, 236}, week[i], Muted, SystemFont::kMedium, true).value();
            auto offset = calendar::Weekday({model_.year, model_.month, 1});
            auto count = calendar::DaysInMonth(model_.year, model_.month);
            for (unsigned d = 1; d <= count; ++d) {
                auto i = offset + d - 1;
                day_values_[i] = d;
                FixedString<8> text;
                text.AppendUint(d);
                bool selected = model_.selected == calendar::Date{model_.year, model_.month, d};
                days_[i] = root_.CreateTextButton({.bounds = {28 + int(i % 7) * 96, 264 + int(i / 7) * 48, 88, 44},
                                                   .text = text.c_str(),
                                                   .style = Style(selected)});
                bool note = false;
                for (auto& n : model_.notes)
                    note |= n && calendar::LocalDate(n->once_utc, model_.offset) ==
                                     calendar::Date{model_.year, model_.month, d};
                if (note)
                    (void)root_
                        .CreateRoundedRect({68 + int(i % 7) * 96, 307 + int(i / 7) * 48, 6, 3},
                                           {.fill = Accent, .radius = 2})
                        .value();
            }
            label_ = root_.CreateLabel({28, 572}, " ", Muted, SystemFont::kMedium).value();
            Button(2, {24, 638, 208, 56}, "回到今天");
            Button(3, {256, 638, 440, 56}, "添加事项", true);
        } else if (model_.page == Notes) {
            unsigned visible = 0;
            for (unsigned i = 0; i < 6; ++i)
                if (model_.notes[i]) {
                    auto y = 166 + int(visible++) * 70;
                    auto& note = *model_.notes[i];
                    Card(root_, {24, y, 672, 64});
                    done_[i] = root_.CreateTextButton({.bounds = {32, y + 6, 52, 52},
                                                       .text = model_.Done(i) ? symbols::kOk : "○",
                                                       .style = Style(model_.Done(i))});
                    auto clip = root_.CreateContainer({.clip = {0, 0, 510, 58}, .translation = {98, y + 3}}).value();
                    (void)clip
                        .CreateLabel({0, 0}, note.label.c_str(), model_.Done(i) ? Muted : Text, SystemFont::kMedium)
                        .value();
                    auto date = calendar::FormatDate(calendar::LocalDate(note.once_utc, model_.offset));
                    FixedString<96> subtitle;
                    subtitle.Append(date.c_str());
                    subtitle.Append("  ");
                    auto time = TimeText(note.once_utc, model_.offset, false);
                    subtitle.Append(time.c_str());
                    subtitle.Append(model_.Done(i) ? " · 已完成" : note.enabled ? " · 待提醒" : " · 已提醒");
                    (void)clip.CreateLabel({0, 32}, subtitle.c_str(), Muted, SystemFont::kSmall).value();
                    edit_[i] = ui::Button({94, y, 514, 64});
                    remove_[i] =
                        root_.CreateTextButton({.bounds = {632, y + 8, 52, 48}, .text = "×", .style = Style()});
                }
            if (!visible) {
                (void)root_.CreateLabel({360, 288}, "给重要的事，留一个提醒", Text, SystemFont::kLarge, true).value();
                (void)root_.CreateLabel({360, 344}, "会议、纪念日，或一件小事", Muted, SystemFont::kMedium, true)
                    .value();
            }
            Button(0, {24, 638, 672, 56}, "+  添加事项", true);
        } else {
            Card(root_, {24, 164, 672, 116});
            Button(0, {40, 184, 640, 76}, model_.draft.empty() ? "点击输入事项名称" : model_.draft.c_str());
            Card(root_, {24, 300, 672, 92});
            Button(1, {36, 318, 60, 56}, symbols::kLeft);
            Button(2, {624, 318, 60, 56}, symbols::kRight);
            label_ = root_.CreateLabel({360, 326}, " ", Text, SystemFont::kLarge, true).value();
            Card(root_, {24, 414, 672, 158});
            digits_ = Digits::Create(root_, {244, 452}, 72, Text);
            Button(3, {40, 428, 88, 52}, "时 -");
            Button(4, {40, 502, 88, 52}, "时 +");
            Button(5, {592, 428, 88, 52}, "分 -");
            Button(6, {592, 502, 88, 52}, "分 +");
            Button(8, {24, 638, 208, 56}, "取消");
            Button(7, {256, 638, 440, 56}, "保存提醒", true);
        }
        status_ = root_.CreateLabel({32, model_.page == Alarm ? 674 : 608}, " ", Muted, SystemFont::kSmall).value();
    }
    Model& model_;
    ContainerNode root_;
    std::array<ui::TextButton, 9> buttons_;
    std::array<ui::TextButton, 42> days_;
    std::array<unsigned, 42> day_values_{};
    std::array<ui::TextButton, 6> done_, remove_;
    std::array<ui::Button, 6> edit_;
    LabelNode label_, status_;
    Digits digits_;
    bool typing_{};
};
class ClockActivity final : public Activity {
   public:
    explicit ClockActivity(Model& model) : model_(model) {}

   private:
    void OnCreate(ComponentContext& context) override {
        const auto& app = context.app();
        model_.Read(app);
        app.renderer()
            .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        scene_.emplace(app.renderer().CreateScene(Background).value());
        (void)scene_->CreateLabel({28, 26}, "日历与提醒", Text, SystemFont::kTitle).value();
        time_ = scene_->CreateLabel({540, 34}, " ", Muted, SystemFont::kLarge).value();
        const char* names[]{"闹钟", "日历", "事项"};
        for (unsigned i = 0; i < 3; ++i)
            tabs_[i] = scene_->CreateTextButton(
                {.bounds = {24 + int(i) * 232, 92, 208, 52}, .text = names[i], .style = Style(i == model_.page)});
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
        for (auto& t : tabs_) t.Reset();
        if (view_) {
            view_->Reset();
            view_->StopInput(c.app());
        }
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        view_.reset();
        scene_.reset();
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if (page_ != model_.page) Build(app);
        bool rebuild = false;
        if (auto* touch = event.touch()) {
            bool tab = false;
            for (unsigned i = 0; i < 3; ++i)
                if (tabs_[i].OnTouch(*touch).clicked) {
                    model_.page = i;
                    model_.title_status.clear();
                    rebuild = tab = true;
                }
            if (!tab && view_) rebuild |= view_->Touch(app, *touch);
        }
        if (view_ && view_->TextInput(app, event)) ++model_.revision;
        if (rebuild) Build(app);
        if (timer_ && event.TimerFrom(*timer_)) {
            auto before = model_.revision;
            model_.Read(app);
            if (page_ != model_.page || (page_ == Notes && before != model_.revision)) Build(app);
        }
        Draw(app);
    }
    void Build(const Application& app) {
        if (view_) view_->StopInput(app);
        view_.reset();
        page_ = model_.page;
        view_ = std::make_unique<PageView>(*scene_, model_);
        app.log().Info(page_ == Calendar ? "CLOCK calendar opened"
                       : page_ == Notes  ? "CLOCK notes opened"
                       : page_ == Editor ? "CLOCK editor opened"
                                         : "CLOCK alarm opened");
    }
    void Draw(const Application& app) {
        auto now = app.alarms().Now().value();
        auto time = TimeText(now.utc, now.offset_minutes, false);
        time_.SetText(now.valid ? time.c_str() : "--:--");
        for (unsigned i = 0; i < 3; ++i)
            tabs_[i].SetStyle(Style(i == (model_.page == Editor ? Notes : model_.page))).value();
        if (view_) view_->Draw(app);
        app.renderer().Present(*scene_).value();
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::optional<Timer> timer_;
    std::array<ui::TextButton, 3> tabs_;
    LabelNode time_;
    std::unique_ptr<PageView> view_;
    unsigned page_{UINT32_MAX};
};
}  // namespace clock_app
int main() {
    micropixel::Application app;
    clock_app::Model model;
    clock_app::ClockActivity page(model);
    clock_app::ClockService service(model);
    app.RunComponents(&page, &service);
    return 0;
}
