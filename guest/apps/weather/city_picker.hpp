// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstring>

#include "cities.hpp"
#include "sdk/ui/material_icons.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/scroll_view.hpp"

namespace weather {
// Reuses 18 buttons regardless of catalog length. SDK ScrollView owns the drag;
// release after scrolling must never select a city underneath the finger.
class CityPicker final {
   public:
    struct Update {
        bool changed{};
        std::optional<uint32_t> city{};
    };
    CityPicker(micropixel::Container& parent, const micropixel::ui::MaterialAtlas& icons) {
        using namespace micropixel;
        using namespace ui::panel;
        root_ = parent.CreateContainer({.z_order = 100, .visible = false}).value();
        (void)root_.CreateShape({0, 0, 720, 720}, Background).value();
        title_ = root_.CreateLabel({96, 21}, "选择城市", Text, SystemFont::kLarge).value();
        back_ = ui::MaterialIconButton::Create(root_, icons, {0, 0, 84, 84}, ui::MaterialSymbol::Back, Background);
        close_ = ui::MaterialIconButton::Create(root_, icons, {612, 0, 84, 84}, ui::MaterialSymbol::Close, Background);
        tabs_[0] = root_.CreateTextButton({.bounds = {24, 96, 324, 84}, .text = "常用城市", .style = Style(true)});
        tabs_[1] = root_.CreateTextButton({.bounds = {366, 96, 330, 84}, .text = "按省份选择", .style = Style()});
        subtitle_ = root_.CreateLabel({24, 195}, " ", Muted, SystemFont::kSmall).value();
        scroll_ = root_.CreateScrollView({24, 228, 672, 456});
        for (auto& row : rows_)
            row = scroll_.content().CreateTextButton({.bounds = {0, 0, 212, 84}, .text = "城市", .style = Style()});
    }
    ~CityPicker() {
        if (root_.valid()) root_.Destroy().value();
    }
    void Show(uint32_t current) {
        current_ = current;
        visible_ = true;
        SetMode(Mode::Popular);
        root_.SetVisible(true);
    }
    bool visible() const { return visible_; }
    void Cancel() {
        scroll_.Cancel();
        back_.Reset();
        close_.Reset();
        for (auto& tab : tabs_) tab.Reset();
        for (auto& row : rows_) row.Reset();
    }
    Update OnTouch(const micropixel::TouchEvent& touch) {
        if (!visible_) return {};
        Update update;
        if (close_.OnTouch(touch).clicked) {
            Hide();
            return {.changed = true};
        }
        if (back_.OnTouch(touch).clicked) {
            if (mode_ == Mode::Cities)
                SetMode(Mode::Provinces);
            else if (mode_ == Mode::Provinces)
                SetMode(Mode::Popular);
            else
                Hide();
            return {.changed = true};
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto tab = tabs_[i].OnTouch(touch);
            update.changed |= tab.visual_changed;
            if (tab.clicked) {
                SetMode(i ? Mode::Provinces : Mode::Popular);
                return {.changed = true};
            }
        }
        auto scroll = scroll_.OnTouch(touch);
        update.changed |= scroll.changed;
        if (scroll.dragging) {
            for (auto& row : rows_) row.Reset();
            BindRows();
            return {.changed = true};
        }
        if (!scroll_.contains(touch.position())) {
            for (auto& row : rows_) row.Reset();
            return update;
        }
        for (unsigned i = 0; i < rows_.size(); ++i) {
            const unsigned item = first_ * 3 + i;
            if (item >= count_) continue;
            auto result = rows_[i].OnTouch(touch);
            update.changed |= result.visual_changed;
            if (result.clicked) {
                if (mode_ == Mode::Provinces) {
                    province_ = item;
                    SetMode(Mode::Cities);
                } else {
                    update.city = items_[item];
                    Hide();
                }
                update.changed = true;
                return update;
            }
        }
        return update;
    }

   private:
    enum class Mode { Popular, Provinces, Cities };
    void Hide() {
        Cancel();
        root_.SetVisible(false);
        visible_ = false;
    }
    void SetMode(Mode mode) {
        using namespace micropixel;
        mode_ = mode;
        Cancel();
        count_ = 0;
        if (mode == Mode::Popular) {
            for (unsigned i = 0; i < PopularCityCount; ++i) items_[count_++] = i;
        } else if (mode == Mode::Provinces)
            count_ = provinces.size();
        else
            for (unsigned i = 0; i < cities.size(); ++i)
                if (cities[i].province == province_) items_[count_++] = i;
        title_.SetText(mode == Mode::Cities ? provinces[province_] : mode == Mode::Provinces ? "选择省份" : "选择城市");
        FixedString<96> text;
        if (mode == Mode::Popular) text.Append("10 个常用城市 · ");
        if (mode == Mode::Cities) {
            text.AppendUint(count_);
            text.Append(" 个城市/地区 · ");
        }
        text.Append("当前：");
        text.Append(cities[current_].name);
        subtitle_.SetText(text.c_str());
        tabs_[0].SetStyle(ui::panel::Style(mode == Mode::Popular)).value();
        tabs_[1].SetStyle(ui::panel::Style(mode != Mode::Popular)).value();
        scroll_.SetContentHeight(int((count_ + 2) / 3) * 96);
        scroll_.ScrollTo(0);
        first_ = UINT32_MAX;
        BindRows();
    }
    void BindRows() {
        using namespace micropixel;
        unsigned first = unsigned(scroll_.offset()) / 96;
        if (first_ == first) return;
        first_ = first;
        for (unsigned i = 0; i < rows_.size(); ++i) {
            unsigned item = first * 3 + i;
            auto& row = rows_[i];
            row.Reset();
            row.SetVisible(item < count_);
            if (item >= count_) continue;
            row.SetBounds({int(i % 3) * 230, int(item / 3) * 96, 212, 84}).value();
            const char* name = mode_ == Mode::Provinces ? provinces[item] : cities[items_[item]].name;
            auto style = ui::panel::Style(mode_ != Mode::Provinces && items_[item] == current_);
            if (std::strlen(name) > 24) style.font = SystemFont::kSmall;
            row.SetStyle(style).value();
            row.SetText(name).value();
        }
    }
    micropixel::ContainerNode root_;
    micropixel::LabelNode title_, subtitle_;
    micropixel::ui::ScrollView scroll_;
    micropixel::ui::MaterialIconButton back_, close_;
    std::array<micropixel::ui::TextButton, 2> tabs_;
    std::array<micropixel::ui::TextButton, 18> rows_;
    std::array<uint32_t, 64> items_{};
    unsigned count_{}, first_{UINT32_MAX}, province_{}, current_{};
    Mode mode_{};
    bool visible_{};
};
}  // namespace weather
