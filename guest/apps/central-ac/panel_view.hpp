// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <span>
#include <utility>

#include "sdk/micropixel.hpp"
#include "sdk/ui/numerals.hpp"
#include "sdk/ui/panel.hpp"

namespace central_ac {
namespace mp = micropixel;
namespace ui = mp::ui;
namespace colors = ui::panel;

inline constexpr int kCanvas = 480;
inline constexpr int kMinTemperature = 10;
inline constexpr int kMaxTemperature = 30;
inline constexpr std::array kModes{"制冷", "制热", "送风"};
inline constexpr std::array kFans{"自动", "低风", "中风", "高风"};
inline constexpr std::array kDurations{"30 分钟", "60 分钟", "120 分钟"};
inline constexpr size_t kNoSelection = kDurations.size();

enum class Page { kHome, kMode, kFan, kTimer };
enum class Preview { kNormal, kLocal, kOffline, kRestricted, kPending, kTimeout };
enum class ClimateIcon { kCool, kHeat, kWind, kTimer };

inline constexpr std::array kModeIcons{ClimateIcon::kCool, ClimateIcon::kHeat, ClimateIcon::kWind};

inline mp::Rect IconSource(ClimateIcon symbol) {
    constexpr int kCell = 48;
    return {static_cast<int>(symbol) * kCell, 0, kCell, kCell};
}

struct PanelState {
    bool powered{true};
    int temperature{24};
    size_t mode{};
    size_t fan{};
    size_t timer{kNoSelection};
    Preview preview{Preview::kNormal};
};

enum class Action { kNone, kLower, kRaise, kPower, kMode, kFan, kTimer, kRefresh, kBack, kSave, kClearTimer, kChoose };

struct TouchResult {
    Action action{Action::kNone};
    bool redraw{};
    size_t option{};
};

inline ui::TextButtonStyle ButtonStyle(bool selected, mp::Color background = colors::Surface, uint32_t radius = 18,
                                       mp::SystemFont font = mp::SystemFont::kMedium) {
    auto style = ui::kPanelTheme.ButtonStyle(selected);
    if (!selected) style.background = background;
    style.corner_radius = radius;
    style.font = font;
    return style;
}

inline ui::TextButtonStyle DisabledStyle(ui::TextButtonStyle style) {
    style.text = colors::Disabled;
    style.disabled_feedback_opacity = 0;
    return style;
}

inline ui::TextButtonStyle StepStyle() { return ButtonStyle(false, colors::Raised, 30, mp::SystemFont::kLarge); }

class ControlTile {
   public:
    void Create(mp::Container& parent, mp::Rect bounds, const char* label) {
        hit_ = ui::Button(bounds);
        root_ = parent.CreateContainer({.translation = {bounds.x, bounds.y}}).value();
        background_ =
            root_
                .CreateRoundedRect({0, 0, bounds.width, bounds.height},
                                   {.fill = colors::Surface, .stroke = colors::Line, .radius = 18, .stroke_width = 1})
                .value();
        center_ = bounds.width / 2;
        label_ = root_.CreateLabel({center_, 35}, label, colors::Muted, mp::SystemFont::kSmall, true).value();
        value_ = root_.CreateLabel({center_, 58}, "", colors::Text, mp::SystemFont::kMedium, true).value();
    }

    void CreateIcon(const mp::Texture& atlas, ClimateIcon symbol) {
        icon_ = root_.CreateSprite(atlas, {center_ - 13, 7, 26, 26}, IconSource(symbol)).value();
    }

    void SetPowerIcon() {
        power_icon_ =
            root_.CreateLabel({center_, 8}, mp::symbols::kPower, colors::Accent, mp::SystemFont::kMedium, true).value();
    }

    void SetIcon(ClimateIcon symbol) { icon_.SetSource(IconSource(symbol)); }

    void Show(const char* value, bool enabled, bool selected) {
        value_.SetText(value);
        hit_.SetEnabled(enabled);
        selected_ = selected;
        label_.SetColor(enabled ? colors::Muted : colors::Disabled);
        value_.SetColor(selected ? colors::Accent : enabled ? colors::Text : colors::Muted);
        background_.SetStrokeColor(selected ? colors::Accent : colors::Line);
        if (power_icon_.valid())
            power_icon_.SetColor(selected ? colors::Accent : colors::Muted);
        else
            icon_.SetOpacity(enabled ? 255 : 105);
        Paint();
    }

    ui::ButtonUpdate Touch(const mp::TouchEvent& event) {
        const auto update = hit_.OnTouch(event);
        if (update.visual_changed) Paint();
        return update;
    }

    void Reset() {
        hit_.Reset();
        Paint();
    }

   private:
    void Paint() {
        background_.SetFillColor(hit_.pressed() ? colors::Line : selected_ ? colors::Raised : colors::Surface);
    }

    ui::Button hit_;
    mp::ContainerNode root_;
    mp::RoundedRectNode background_;
    mp::LabelNode label_, value_, power_icon_;
    mp::SpriteNode icon_;
    int center_{};
    bool selected_{};
};

class HomeView {
   public:
    HomeView(mp::Scene& scene, const ui::NumeralAtlas& numbers, const mp::Texture& icons) {
        using enum mp::SystemFont;
        root_ = scene.CreateContainer().value();
        (void)root_.CreateLabel({22, 17}, "中央空调", colors::Muted, kSmall).value();
        (void)root_.CreateLabel({20, 45}, "客厅", colors::Text, kTitle).value();
        (void)root_.CreateRoundedRect({340, 40, 120, 38}, {.fill = colors::Raised, .radius = 19}).value();
        badge_dot_ = root_.CreateRoundedRect({352, 55, 8, 8}, {.fill = colors::Blue, .radius = 4}).value();
        badge_ = root_.CreateLabel({405, 47}, "", colors::Text, kSmall, true).value();

        (void)root_.CreateRoundedRect({20, 100, 440, 228}, {.fill = colors::Surface, .radius = 24}).value();
        caption_ = root_.CreateLabel({240, 118}, "", colors::Blue, kMedium, true).value();
        temperature_ = ui::Numerals::Create(root_, numbers, {165, 141}, 114, 2);
        unit_ = root_.CreateLabel({310, 161}, "°C", colors::Muted, kLarge, true).value();
        temperature_state_ = root_.CreateLabel({240, 198}, "", colors::Text, kTitle, true).value();
        (void)root_.CreateRoundedRect({156, 268, 168, 38}, {.fill = colors::Raised, .radius = 19}).value();
        room_ = root_.CreateLabel({240, 275}, "", colors::Muted, kSmall, true).value();
        lower_ = root_.CreateTextButton(
            {.bounds = {48, 175, 60, 60}, .text = mp::symbols::kMinus, .style = StepStyle(), .hit_padding = 8});
        raise_ = root_.CreateTextButton(
            {.bounds = {372, 175, 60, 60}, .text = mp::symbols::kPlus, .style = StepStyle(), .hit_padding = 8});

        constexpr std::array labels{"电源", "模式", "风速", "定时"};
        for (size_t i = 0; i < tiles_.size(); ++i) {
            tiles_[i].Create(root_, {20 + static_cast<int>(i) * 112, 342, 104, 88}, labels[i]);
        }
        tiles_[0].SetPowerIcon();
        tiles_[1].CreateIcon(icons, kModeIcons[0]);
        tiles_[2].CreateIcon(icons, ClimateIcon::kWind);
        tiles_[3].CreateIcon(icons, ClimateIcon::kTimer);

        (void)root_.CreateShape({20, 440, 440, 1}, colors::Line).value();
        status_ = root_.CreateLabel({22, 450}, "", colors::Muted, kSmall).value();
        preview_ = root_.CreateLabel({419, 450}, "界面预览", colors::Muted, kSmall, true).value();
        refresh_ = root_.CreateTextButton({.bounds = {342, 445, 118, 31},
                                           .text = "刷新状态",
                                           .style = ButtonStyle(false, colors::Raised, 12, kSmall)});
    }

    void Show(const PanelState& state) {
        const bool offline = state.preview == Preview::kOffline;
        const bool timeout = state.preview == Preview::kTimeout;
        const bool pending = state.preview == Preview::kPending;
        const bool unknown = offline || timeout;
        const bool enabled = !unknown && !pending;
        const bool running = state.powered && !unknown;
        const bool numeric = running && state.mode < 2;
        const bool adjust = enabled && numeric;

        badge_.SetText(offline                            ? "设备离线"
                       : timeout                          ? "待确认"
                       : state.preview == Preview::kLocal ? "本地可控"
                                                          : "设备在线");
        badge_dot_.SetFillColor(unknown ? colors::Accent : colors::Blue);
        caption_.SetText(pending   ? "目标设定（待确认）"
                         : timeout ? "当前设定未确认"
                         : numeric ? "设定温度"
                                   : "空调状态");
        caption_.SetColor(!numeric ? colors::Muted : state.mode == 0 ? colors::Blue : colors::Accent);
        mp::FixedString<4> temperature;
        if (numeric) temperature.AppendInt(state.temperature + (pending ? 1 : 0));
        temperature_.Set(temperature.c_str());
        unit_.SetVisible(numeric);
        temperature_state_.SetText(unknown ? "—" : state.powered ? "送风" : "已关机");
        temperature_state_.SetVisible(!numeric);
        room_.SetText(offline ? "室温不可用" : "室温 26 °C");
        SetStepEnabled(lower_, adjust && state.temperature > kMinTemperature);
        SetStepEnabled(raise_, adjust && state.temperature < kMaxTemperature);

        const char* power = state.powered ? "已开启" : "已关闭";
        const bool timed = state.timer != kNoSelection;
        tiles_[0].Show(unknown ? "—" : power, enabled, running);
        tiles_[1].SetIcon(kModeIcons[state.mode]);
        tiles_[1].Show(unknown ? "—" : kModes[state.mode], enabled && running, false);
        tiles_[2].Show(unknown ? "—" : kFans[state.fan], enabled && running, false);
        tiles_[3].Show(unknown ? "—"
                       : timed ? kDurations[state.timer]
                               : "未设置",
                       enabled && running, timed && running);

        mp::FixedString<48> running_status;
        running_status.Append("设备已开启 · ");
        running_status.Append(kModes[state.mode]);
        running_status.Append("模式");
        const char* status = state.powered ? running_status.c_str() : "设备已关闭";
        if (pending) status = "正在等待设备确认…";
        if (timeout) status = "指令超时，结果未知";
        if (offline) status = "请检查空调控制链路";
        status_.SetText(status);
        status_.SetColor(unknown || pending ? colors::Accent : colors::Muted);
        preview_.SetVisible(!timeout);
        refresh_.SetVisible(timeout);
    }

    void SetVisible(bool visible) { root_.SetVisible(visible); }

    TouchResult Touch(const mp::TouchEvent& event) {
        constexpr std::array actions{Action::kPower, Action::kMode, Action::kFan, Action::kTimer};
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const auto update = tiles_[i].Touch(event);
            if (update.handled) return {update.clicked ? actions[i] : Action::kNone, update.redraw()};
        }
        for (auto [button, action] : {std::pair{&lower_, Action::kLower}, std::pair{&raise_, Action::kRaise},
                                      std::pair{&refresh_, Action::kRefresh}}) {
            if (!button->visible()) continue;
            const auto update = button->OnTouch(event);
            if (update.handled) return {update.clicked ? action : Action::kNone, update.redraw()};
        }
        return {};
    }

    void Reset() {
        for (auto& tile : tiles_) tile.Reset();
        for (auto* button : {&lower_, &raise_, &refresh_}) button->Reset();
    }

   private:
    static void SetStepEnabled(ui::TextButton& button, bool enabled) {
        if (button.enabled() == enabled) return;
        button.SetEnabled(enabled);
        button.SetStyle(enabled ? StepStyle() : DisabledStyle(StepStyle())).value();
    }

    mp::ContainerNode root_;
    mp::RoundedRectNode badge_dot_;
    mp::LabelNode badge_, caption_, unit_, temperature_state_, room_, status_, preview_;
    ui::Numerals temperature_;
    ui::TextButton lower_, raise_, refresh_;
    std::array<ControlTile, 4> tiles_;
};

class PickerView {
   public:
    explicit PickerView(mp::Scene& scene) {
        using enum mp::SystemFont;
        root_ = scene.CreateContainer({.visible = false}).value();
        title_ = root_.CreateLabel({20, 21}, "选择风速", colors::Text, kTitle).value();
        help_ = root_.CreateLabel({22, 70}, "点选后返回主界面", colors::Muted, kSmall).value();
        back_ = root_.CreateTextButton(
            {.bounds = {360, 16, 100, 48}, .text = "返回", .style = ButtonStyle(false, colors::Raised)});
        for (size_t i = 0; i < choices_.size(); ++i) {
            choices_[i] = root_.CreateTextButton({.bounds = {20, 109 + static_cast<int>(i) * 72, 440, 62},
                                                  .text = kFans[i],
                                                  .style = ButtonStyle(false)});
        }
        save_ = root_.CreateTextButton({.bounds = {20, 409, 440, 51}, .text = "保存", .style = ButtonStyle(true)});
        clear_ = root_.CreateTextButton(
            {.bounds = {20, 409, 146, 51}, .text = "取消定时", .style = ButtonStyle(false, colors::Raised)});
    }

    void Show(Page page, const PanelState& state, size_t draft) {
        const bool mode = page == Page::kMode;
        const bool timer = page == Page::kTimer;
        const bool restricted = mode && state.preview == Preview::kRestricted;
        const bool timed = timer && state.timer != kNoSelection;
        const size_t selected = mode ? state.mode : timer ? draft : state.fan;
        const std::span<const char* const> options = mode    ? std::span<const char* const>(kModes)
                                                     : timer ? std::span<const char* const>(kDurations)
                                                             : std::span<const char* const>(kFans);
        title_.SetText(mode ? "选择模式" : timer ? "定时关机" : "选择风速");
        help_.SetText(restricted ? "制热受集中控制限制" : timer ? "选择关机时长后保存" : "点选后返回主界面");
        help_.SetColor(restricted ? colors::Accent : colors::Muted);
        for (size_t i = 0; i < choices_.size(); ++i) {
            auto& choice = choices_[i];
            choice.SetVisible(i < options.size());
            if (i >= options.size()) continue;
            const bool enabled = !restricted || i != 1;
            choice.SetText(options[i]).value();
            choice.SetStyle(enabled ? ButtonStyle(i == selected) : DisabledStyle(ButtonStyle(false))).value();
            choice.SetEnabled(enabled);
        }
        clear_.SetVisible(timed);
        save_.SetVisible(timer);
        save_.SetBounds(timed ? mp::Rect{178, 409, 282, 51} : mp::Rect{20, 409, 440, 51}).value();
        const bool can_save = draft != kNoSelection;
        save_.SetStyle(can_save ? ButtonStyle(true) : DisabledStyle(ButtonStyle(false))).value();
        save_.SetEnabled(can_save);
        root_.SetVisible(true);
    }

    void Hide() {
        Reset();
        root_.SetVisible(false);
    }

    TouchResult Touch(const mp::TouchEvent& event) {
        for (auto [button, action] : {std::pair{&back_, Action::kBack}, std::pair{&save_, Action::kSave},
                                      std::pair{&clear_, Action::kClearTimer}}) {
            if (!button->visible()) continue;
            const auto update = button->OnTouch(event);
            if (update.handled) return {update.clicked ? action : Action::kNone, update.redraw()};
        }
        for (size_t i = 0; i < choices_.size(); ++i) {
            if (!choices_[i].visible()) continue;
            const auto update = choices_[i].OnTouch(event);
            if (update.handled) return {update.clicked ? Action::kChoose : Action::kNone, update.redraw(), i};
        }
        return {};
    }

    void Reset() {
        for (auto& choice : choices_) choice.Reset();
        for (auto* button : {&back_, &save_, &clear_}) button->Reset();
    }

   private:
    mp::ContainerNode root_;
    mp::LabelNode title_, help_;
    std::array<ui::TextButton, kFans.size()> choices_;
    ui::TextButton back_, save_, clear_;
};
}  // namespace central_ac
