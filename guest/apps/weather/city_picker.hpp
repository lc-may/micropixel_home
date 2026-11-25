// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstring>

#include "cities.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/panel.hpp"

namespace weather {
class CityPicker final {
    enum class Mode { Popular, Provinces, Cities };
    auto Key() const {
        return [this](uint32_t index) -> uint64_t {
            return (uint64_t(mode_) << 32) | (mode_ == Mode::Provinces ? index : items_[index]);
        };
    }
    auto Binder() {
        return [this](auto& row, uint32_t item, micropixel::Rect bounds) {
            const char* name = mode_ == Mode::Provinces ? provinces[item] : cities[items_[item]].name;
            auto style = micropixel::ui::panel::Style(mode_ != Mode::Provinces && items_[item] == current_);
            if (std::strlen(name) > 24) style.font = micropixel::SystemFont::kSmall;
            row.SetBounds(bounds).value();
            row.SetStyle(style).value();
            row.SetText(name).value();
        };
    }

   public:
    struct Update {
        bool changed{};
        std::optional<uint32_t> city{};
    };
    CityPicker(micropixel::Container& parent, const micropixel::ui::MaterialAtlas& icons) {
        using namespace micropixel;
        using namespace ui::panel;
        root_ = ui::ComponentRoot(parent.CreateContainer({.z_order = 100, .visible = false}).value());
        (void)root_.node().CreateShape({0, 0, 720, 720}, Background).value();
        const ui::AppBarAction actions[] = {{.icon = ui::MaterialSymbol::Close}};
        header_ = ui::AppBar::Create(root_.node(),
                                     {.title = "选择城市",
                                      .leading = ui::AppBarAction{.icon = ui::MaterialSymbol::Back},
                                      .actions = actions},
                                     &icons)
                      .value();
        const char* labels[] = {"常用城市", "按省份选择"};
        auto tabs = ui::SegmentedControl<2>::Create(root_.node(), {24, 96, 672, 84}, labels);
        Assert(tabs.has_value(), "city tabs creation failed");
        tabs_ = std::move(*tabs);
        subtitle_ = root_.node().CreateLabel({24, 195}, " ", Muted, SystemFont::kSmall).value();
        auto list = ui::VirtualList<ui::TextButton, 18>::Create(
            root_.node(),
            {.bounds = {24, 228, 672, 456},
             .item_width = 212,
             .item_height = 84,
             .column_gap = 18,
             .row_gap = 12,
             .columns = 3},
            [](Container& content, Rect bounds) {
                return content.CreateTextButton({.bounds = bounds, .text = "城市", .style = Style()});
            });
        Assert(list.has_value(), "city list creation failed");
        list_ = std::move(*list);
    }
    void Show(uint32_t current) {
        current_ = current;
        visible_ = true;
        SetMode(Mode::Popular);
        root_.node().SetVisible(true);
    }
    bool visible() const { return visible_; }
    bool Back() {
        if (!visible_) return false;
        if (mode_ == Mode::Cities)
            SetMode(Mode::Provinces);
        else if (mode_ == Mode::Provinces)
            SetMode(Mode::Popular);
        else
            Hide();
        return true;
    }
    void Cancel() {
        header_.CancelInput();
        tabs_.CancelInput();
        list_.CancelInput();
    }
    Update OnTouch(const micropixel::TouchEvent& touch) {
        if (!visible_) return {};
        auto header = header_.OnTouch(touch);
        if (header.action) {
            if (header.action->kind == micropixel::ui::AppBarActionKind::kLeading)
                Back();
            else
                Hide();
            return {.changed = true};
        }
        if (header.handled) return {.changed = header.changed};
        auto tab = tabs_.OnTouch(touch);
        if (tab.action) {
            SetMode(*tab.action ? Mode::Provinces : Mode::Popular);
            return {.changed = true};
        }
        if (tab.handled) return {.changed = tab.changed};
        auto update = list_.OnTouch(touch, Key(), Binder(), [](auto& row, const auto& event) {
            auto result = row.OnTouch(event);
            return micropixel::ui::RowUpdate{result.visual_changed, result.clicked};
        });
        if (update.action) {
            if (mode_ == Mode::Provinces) {
                province_ = update.action->index;
                SetMode(Mode::Cities);
            } else {
                auto city = items_[update.action->index];
                Hide();
                return {.changed = true, .city = city};
            }
            return {.changed = true};
        }
        return {.changed = update.changed};
    }

   private:
    void Hide() {
        Cancel();
        root_.node().SetVisible(false);
        visible_ = false;
    }
    void SetMode(Mode mode) {
        using namespace micropixel;
        Cancel();
        mode_ = mode;
        count_ = 0;
        if (mode == Mode::Popular) {
            for (unsigned i = 0; i < PopularCityCount; ++i) items_[count_++] = i;
        } else if (mode == Mode::Provinces)
            count_ = provinces.size();
        else
            for (unsigned i = 0; i < cities.size(); ++i)
                if (cities[i].province == province_) items_[count_++] = i;
        header_
            .SetTitle(mode == Mode::Cities      ? provinces[province_]
                      : mode == Mode::Provinces ? "选择省份"
                                                : "选择城市")
            .value();
        FixedString<96> text;
        if (mode == Mode::Popular) text.Append("10 个常用城市 · ");
        if (mode == Mode::Cities) {
            text.AppendUint(count_);
            text.Append(" 个城市/地区 · ");
        }
        text.Append("当前：");
        text.Append(cities[current_].name);
        subtitle_.SetText(text.c_str());
        tabs_.SetSelected(mode == Mode::Popular ? 0 : 1);
        list_.SetItems(count_, ++generation_, true).value();
        list_.BindRows(Key(), Binder());
    }
    micropixel::ui::ComponentRoot root_;
    micropixel::ui::AppBar header_;
    micropixel::LabelNode subtitle_;
    micropixel::ui::SegmentedControl<2> tabs_;
    micropixel::ui::VirtualList<micropixel::ui::TextButton, 18> list_;
    std::array<uint32_t, 64> items_{};
    unsigned count_{}, province_{}, current_{}, generation_{};
    Mode mode_{};
    bool visible_{};
};
}  // namespace weather
