// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "planner.hpp"
#include "sdk/ui/material_icons.hpp"
#include "sdk/ui/numerals.hpp"
#include "sdk/ui/panel.hpp"
namespace clock_app {
using namespace ui::panel;
inline constexpr unsigned AlarmEditor = 4;
class ClockService final : public AppService {
   public:
    explicit ClockService(Model& model) : model_(model) {}

   private:
    void OnStart(ComponentContext& c) override {
        (void)c.app().widgets().Publish(
            1, {.kind = WidgetTemplate::kClock, .title = "日历与提醒", .subtitle = "时钟、闹钟与日程"});
        c.app().log().Info("CLOCK service ready");
    }
    void OnEvent(ComponentContext& c, const Event& event) override {
        if (auto* incoming = event.message()) {
            auto message = c.app().messages().Read(incoming->ticket);
            int status = -2;
            if (message && message->type == static_cast<uint16_t>(AppAction::kOpenPage)) {
                auto page = message->U32();
                if (page && *page <= Notes) {
                    model_.page = *page;
                    ++model_.revision;
                    status = 0;
                    c.app().log().Info("CLOCK page requested");
                }
            }
            (void)c.app().messages().Ack(incoming->ticket, status);
        }
    }
    Model& model_;
};
class PageView {
   public:
    PageView(Scene& scene, Model& model, const ui::NumeralAtlas& font, const ui::MaterialAtlas& icons)
        : model_(model), font_(font), icons_(icons) {
        root_ = scene.CreateContainer().value();
        Build();
    }
    ~PageView() {
        if (root_.valid()) root_.Destroy().value();
    }
    void Reset() {
        for (auto& b : buttons_)
            if (b.valid()) b.Reset();
        for (auto& b : icon_buttons_)
            if (b.valid()) b.Reset();
        for (auto& b : days_)
            if (b.valid()) b.Reset();
        for (auto& b : done_)
            if (b.valid()) b.Reset();
        for (auto& b : remove_)
            if (b.valid()) b.Reset();
        for (auto& b : edit_) b.Reset();
        for (auto& b : reminder_hits_) b.Reset();
        hero_.Reset();
        alarm_row_.Reset();
        toggle_.Reset();
        row_toggle_.Reset();
    }
    bool Touch(const Application& app, const TouchEvent& touch) {
        bool rebuild = false;
        if (model_.page == Alarm) {
            if (hero_.OnTouch(touch).clicked || alarm_row_.OnTouch(touch).clicked) {
                model_.page = AlarmEditor;
                rebuild = true;
            }
            if (toggle_.Touch(touch) || row_toggle_.Touch(touch)) {
                auto saved = app.alarms().Get(1);
                auto plan = saved ? *saved : model_.Plan(app.alarms().Now().value());
                plan.enabled = !(saved && saved->enabled);
                if (plan.enabled && !plan.weekly && plan.once_utc <= app.alarms().Now().value().utc)
                    plan = model_.Plan(app.alarms().Now().value());
                auto result = app.alarms().Register(plan);
                model_.title_status = result ? "闹钟已更新" : "更新失败，请重试";
                app.log().Info(result ? "CLOCK saved" : "CLOCK save failed");
            }
            if (Hit(0, touch)) {
                model_.page = Calendar;
                rebuild = true;
            }
            if (Hit(1, touch)) {
                model_.page = Notes;
                rebuild = true;
            }
            for (unsigned i = 0; i < 2; ++i)
                if (reminder_hits_[i].OnTouch(touch).clicked && reminder_slots_[i] >= 0) {
                    model_.Edit(unsigned(reminder_slots_[i]));
                    rebuild = true;
                }
        } else if (model_.page == AlarmEditor) {
            if (Hit(0, touch)) model_.hour = (model_.hour + 23) % 24;
            if (Hit(1, touch)) model_.hour = (model_.hour + 1) % 24;
            if (Hit(2, touch)) model_.minute = (model_.minute + 59) % 60;
            if (Hit(3, touch)) model_.minute = (model_.minute + 1) % 60;
            if (Hit(4, touch)) model_.daily = !model_.daily;
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
            if (Hit(8, touch)) {
                auto result = app.alarms().Register(model_.Plan(app.alarms().Now().value()));
                model_.title_status = result ? "闹钟已保存" : "保存失败，请检查时间";
                app.log().Info(result ? "CLOCK saved" : "CLOCK save failed");
                if (result) {
                    model_.page = Alarm;
                    rebuild = true;
                }
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
            if (Hit(1, touch))
                model_.draft_date = calendar::FromDays(std::max<int64_t>(0, calendar::Days(model_.draft_date) - 1));
            if (Hit(2, touch)) model_.draft_date = calendar::FromDays(calendar::Days(model_.draft_date) + 1);
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
            digits_.Set(saved ? time.c_str() : "--:--");
            row_number_.Set(time.c_str());
            row_number_.SetOpacity(model_.enabled ? 255 : 100);
            toggle_.Set(model_.enabled);
            row_toggle_.Set(model_.enabled);
            FixedString<96> subtitle;
            uint64_t next = 0;
            if (model_.enabled) {
                if (!saved->weekly)
                    next = saved->once_utc;
                else
                    for (unsigned i = 0; i < 8 && !next; ++i) {
                        auto day = calendar::FromDays(calendar::Days(calendar::LocalDate(now.utc, model_.offset)) + i);
                        auto t = calendar::Utc(day, saved->minute_of_day, model_.offset);
                        if ((saved->weekdays & (1U << calendar::Weekday(day))) && t > static_cast<int64_t>(now.utc))
                            next = uint64_t(t);
                    }
            }
            if (next > now.utc) {
                auto minutes = (next - now.utc + 59) / 60;
                subtitle.Append("还有 ");
                if (minutes >= 60) {
                    subtitle.AppendUint(minutes / 60);
                    subtitle.Append(" 小时 ");
                }
                subtitle.AppendUint(minutes % 60);
                subtitle.Append(" 分 · ");
                subtitle.Append(model_.daily ? "每天" : "仅一次");
            } else
                subtitle.Append("点击时间设置闹钟");
            label_.SetText(subtitle.c_str());
            row_subtitle_.SetText(model_.enabled ? (model_.daily ? "每天 · 已开启" : "单次 · 已开启")
                                                 : "已关闭 · 点按设置");
        } else if (model_.page == AlarmEditor) {
            auto time = calendar::FormatTime(model_.hour * 60 + model_.minute);
            digits_.Set(time.c_str());
            buttons_[4].SetText(model_.daily ? "每天重复" : "仅一次").value();
            buttons_[4].SetStyle(Style(model_.daily)).value();
            buttons_[6].SetText(model_.offset == 480 ? "时区 UTC+8" : "时区 UTC").value();
        } else if (model_.page == Editor) {
            buttons_[0].SetText(model_.draft.empty() ? "点击输入事项名称" : model_.draft.c_str()).value();
            label_.SetText(calendar::FormatDate(model_.draft_date).c_str());
            digits_.Set(calendar::FormatTime(model_.draft_minute).c_str());
        }
        if (status_.valid()) status_.SetText(model_.title_status.empty() ? " " : model_.title_status.c_str());
    }

   private:
    bool Hit(unsigned i, const TouchEvent& t) {
        return icon_buttons_[i].valid() ? icon_buttons_[i].OnTouch(t).clicked
                                        : buttons_[i].valid() && buttons_[i].OnTouch(t).clicked;
    }
    void Button(unsigned i, Rect r, const char* text, bool active = false) {
        if (std::string_view(text) == symbols::kLeft || std::string_view(text) == symbols::kRight)
            icon_buttons_[i] = ui::MaterialIconButton::Create(
                root_, icons_, r,
                std::string_view(text) == symbols::kLeft ? ui::MaterialSymbol::Left : ui::MaterialSymbol::Right);
        else
            buttons_[i] = root_.CreateTextButton({.bounds = r, .text = text, .style = Style(active)});
    }
    void Build() {
        if (model_.page == Alarm) {
            (void)root_.CreateRoundedRect({24, 84, 672, 228}, {.fill = Surface, .radius = 33}).value();
            (void)root_.CreateLabel({54, 106}, "下次闹钟", Muted, SystemFont::kSmall).value();
            digits_ = ui::Numerals::Create(root_, font_, {54, 137}, 96);
            label_ = root_.CreateLabel({54, 260}, " ", Accent, SystemFont::kMedium).value();
            hero_ = ui::Button({24, 84, 525, 228});
            toggle_ = Switch::Create(root_, {579, 100, 84, 84});
            row_number_ = ui::Numerals::Create(root_, font_, {24, 333}, 45);
            row_subtitle_ = root_.CreateLabel({210, 354}, " ", Muted, SystemFont::kMedium).value();
            row_toggle_ = Switch::Create(root_, {588, 327, 84, 84});
            alarm_row_ = ui::Button({24, 327, 540, 84});
            (void)root_.CreateShape({24, 410, 672, 1}, Line).value();
            unsigned row = 0;
            for (unsigned i = 0; i < 6 && row < 2; ++i)
                if (model_.notes[i] && !model_.Done(i)) {
                    int y = 411 + int(row) * 84;
                    auto& note = *model_.notes[i];
                    auto number = ui::Numerals::Create(root_, font_, {24, y + 6}, 45);
                    number.Set(calendar::FormatTime(
                                   uint32_t(calendar::LocalSeconds(note.once_utc, model_.offset) % 86400) / 60)
                                   .c_str());
                    auto clip = root_.CreateContainer({.clip = {0, 0, 360, 76}, .translation = {210, y + 6}}).value();
                    (void)clip.CreateLabel({0, 0}, note.label.c_str(), Text, SystemFont::kMedium).value();
                    (void)clip
                        .CreateLabel({0, 36}, note.enabled ? "事项 · 待提醒" : "事项 · 已提醒", Muted,
                                     SystemFont::kSmall)
                        .value();
                    (void)root_.CreateLabel({642, y + 26}, symbols::kRight, Muted, SystemFont::kLarge, true).value();
                    reminder_hits_[row] = ui::Button({24, y, 672, 84});
                    reminder_slots_[row] = int(i);
                    ++row;
                }
            if (!row) {
                (void)root_.CreateLabel({24, 453}, "让每一件重要的事，都被记得", Muted, SystemFont::kLarge).value();
                (void)root_.CreateLabel({24, 498}, "从日历或事项添加一个提醒", Muted, SystemFont::kMedium).value();
            }
            Button(0, {24, 615, 324, 84}, "日历");
            Button(1, {366, 615, 330, 84}, "事项");
        } else if (model_.page == AlarmEditor) {
            Card(root_, {24, 96, 672, 200});
            digits_ = ui::Numerals::Create(root_, font_, {54, 116}, 96);
            (void)root_.CreateLabel({54, 250}, "设置提醒时间", Muted, SystemFont::kMedium).value();
            const char* names[]{"小时 −", "小时 +", "分钟 −", "分钟 +"};
            for (unsigned i = 0; i < 4; ++i) Button(i, {24 + int(i) * 174, 315, 150, 84}, names[i]);
            Button(4, {24, 417, 324, 84}, "每天重复", true);
            Button(5, {366, 417, 330, 84}, "10 秒后提醒");
            Button(6, {24, 519, 324, 84}, "时区 UTC+8");
            Button(7, {366, 519, 330, 84}, "同步时间");
            Button(8, {24, 615, 672, 84}, "保存并启用", true);
        } else if (model_.page == Calendar) {
            FixedString<40> text;
            text.AppendInt(model_.year);
            text.Append(" 年 ");
            text.AppendUint(model_.month);
            text.Append(" 月");
            Button(0, {24, 75, 84, 84}, symbols::kLeft);
            Button(1, {612, 75, 84, 84}, symbols::kRight);
            Button(2, {114, 75, 492, 84}, text.c_str());
            constexpr const char* week[]{"一", "二", "三", "四", "五", "六", "日"};
            for (unsigned i = 0; i < 7; ++i)
                (void)root_.CreateLabel({72 + int(i) * 96, 160}, week[i], Muted, SystemFont::kMedium, true).value();
            auto offset = calendar::Weekday({model_.year, model_.month, 1});
            auto count = calendar::DaysInMonth(model_.year, model_.month);
            for (unsigned d = 1; d <= count; ++d) {
                auto i = offset + d - 1;
                day_values_[i] = d;
                FixedString<8> number;
                number.AppendUint(d);
                bool selected = model_.selected == calendar::Date{model_.year, model_.month, d};
                auto style = Style(selected);
                style.corner_radius = 27;
                style.font = SystemFont::kLarge;
                days_[i] = root_.CreateTextButton({.bounds = {30 + int(i % 7) * 96, 194 + int(i / 7) * 84, 84, 84},
                                                   .text = number.c_str(),
                                                   .style = style});
                for (auto& n : model_.notes)
                    if (n && calendar::LocalDate(n->once_utc, model_.offset) ==
                                 calendar::Date{model_.year, model_.month, d}) {
                        (void)root_
                            .CreateRoundedRect({68 + int(i % 7) * 96, 263 + int(i / 7) * 84, 8, 4},
                                               {.fill = selected ? OnAccent : Accent, .radius = 2})
                            .value();
                        break;
                    }
            }
        } else if (model_.page == Notes) {
            unsigned visible = 0;
            for (unsigned i = 0; i < 6; ++i)
                if (model_.notes[i]) {
                    int y = 96 + int(visible++) * 84;
                    auto& note = *model_.notes[i];
                    Card(root_, {24, y, 672, 78});
                    done_[i] = ui::MaterialIconButton::Create(
                        root_, icons_, {24, y, 84, 78},
                        model_.Done(i) ? ui::MaterialSymbol::Done : ui::MaterialSymbol::Unchecked);
                    auto clip = root_.CreateContainer({.clip = {0, 0, 492, 72}, .translation = {120, y + 6}}).value();
                    (void)clip
                        .CreateLabel({0, 0}, note.label.c_str(), model_.Done(i) ? Muted : Text, SystemFont::kLarge)
                        .value();
                    FixedString<96> subtitle;
                    subtitle.Append(calendar::FormatDate(calendar::LocalDate(note.once_utc, model_.offset)).c_str());
                    subtitle.Append("  ");
                    subtitle.Append(TimeText(note.once_utc, model_.offset, false).c_str());
                    subtitle.Append(model_.Done(i) ? " · 已完成" : note.enabled ? " · 待提醒" : " · 已提醒");
                    (void)clip.CreateLabel({0, 38}, subtitle.c_str(), Muted, SystemFont::kSmall).value();
                    edit_[i] = ui::Button({114, y, 498, 78});
                    remove_[i] =
                        ui::MaterialIconButton::Create(root_, icons_, {612, y, 84, 78}, ui::MaterialSymbol::Delete);
                }
            if (!visible) {
                (void)root_.CreateLabel({360, 282}, "给重要的事，留一个提醒", Text, SystemFont::kLarge, true).value();
                (void)root_.CreateLabel({360, 333}, "会议、纪念日，或一件小事", Muted, SystemFont::kMedium, true)
                    .value();
            }
            Button(0, {24, 615, 672, 84}, "+ 添加事项", true);
        } else {
            Card(root_, {24, 96, 672, 114});
            Button(0, {24, 96, 672, 114}, model_.draft.empty() ? "点击输入事项名称" : model_.draft.c_str());
            Card(root_, {24, 234, 672, 102});
            Button(1, {24, 243, 84, 84}, symbols::kLeft);
            Button(2, {612, 243, 84, 84}, symbols::kRight);
            label_ = root_.CreateLabel({360, 269}, " ", Text, SystemFont::kLarge, true).value();
            Card(root_, {24, 360, 672, 222});
            digits_ = ui::Numerals::Create(root_, font_, {228, 384}, 96);
            Button(3, {24, 366, 84, 84}, "时 −");
            Button(4, {24, 480, 84, 84}, "时 +");
            Button(5, {612, 366, 84, 84}, "分 −");
            Button(6, {612, 480, 84, 84}, "分 +");
            Button(8, {24, 615, 208, 84}, "取消");
            Button(7, {250, 615, 446, 84}, "保存提醒", true);
        }
        if (model_.page == Editor) status_ = root_.CreateLabel({24, 588}, " ", Muted, SystemFont::kSmall).value();
    }
    Model& model_;
    const ui::NumeralAtlas& font_;
    const ui::MaterialAtlas& icons_;
    std::array<ui::MaterialIconButton, 9> icon_buttons_;
    ContainerNode root_;
    std::array<ui::TextButton, 9> buttons_;
    std::array<ui::TextButton, 42> days_;
    std::array<unsigned, 42> day_values_{};
    std::array<ui::MaterialIconButton, 6> done_, remove_;
    std::array<ui::Button, 6> edit_;
    std::array<ui::Button, 2> reminder_hits_;
    std::array<int, 2> reminder_slots_{-1, -1};
    ui::Button hero_, alarm_row_;
    Switch toggle_, row_toggle_;
    LabelNode label_, status_, row_subtitle_;
    ui::Numerals digits_, row_number_;
    bool typing_{};
};
class ClockActivity final : public Activity {
   public:
    explicit ClockActivity(Model& model) : model_(model) {}

   private:
    void OnCreate(ComponentContext& c) override {
        const auto& app = c.app();
        model_.Read(app);
        app.renderer()
            .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        scene_.emplace(app.renderer().CreateScene(Background).value());
        font_ = std::make_unique<ui::NumeralAtlas>(app, Text, 96);
        icons_ = std::make_unique<ui::MaterialAtlas>(app);
        title_ = scene_->CreateLabel({24, 21}, "闹钟", Text, SystemFont::kLarge).value();
        time_ = scene_->CreateLabel({490, 23}, " ", Muted, SystemFont::kMedium).value();
        back_ = ui::MaterialIconButton::Create(*scene_, *icons_, {0, 0, 84, 84}, ui::MaterialSymbol::Back, Background);
        add_ = ui::MaterialIconButton::Create(*scene_, *icons_, {612, 0, 84, 84}, ui::MaterialSymbol::Add, Background);
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
        back_.Reset();
        add_.Reset();
        if (view_) {
            view_->Reset();
            view_->StopInput(c.app());
        }
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        view_.reset();
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
            if (model_.page != Alarm && back_.OnTouch(*touch).clicked) {
                model_.page = model_.page == Editor ? Notes : Alarm;
                rebuild = header = true;
            }
            if ((model_.page == Alarm || model_.page == Calendar || model_.page == Notes) &&
                add_.OnTouch(*touch).clicked) {
                if (model_.page == Alarm) {
                    model_.page = AlarmEditor;
                    rebuild = true;
                } else
                    rebuild = model_.New(app);
                header = true;
            }
            if (!header && view_) rebuild |= view_->Touch(app, *touch);
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
        time_.SetText(now.valid ? TimeText(now.utc, now.offset_minutes, false).c_str() : "--:--");
        const char* titles[]{"闹钟", "日历", "事项", "编辑提醒", "设置闹钟"};
        title_.SetText(titles[model_.page]);
        title_.SetPosition({model_.page == Alarm ? 24 : 96, 21});
        back_.SetVisible(model_.page != Alarm);
        add_.SetVisible(model_.page <= Notes);
        if (view_) view_->Draw(app);
        app.renderer().Present(*scene_).value();
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::unique_ptr<ui::NumeralAtlas> font_;
    std::unique_ptr<ui::MaterialAtlas> icons_;
    std::optional<Timer> timer_;
    ui::MaterialIconButton back_, add_;
    LabelNode title_, time_;
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
