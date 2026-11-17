// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <span>
#include <string>

#include "sdk/ui/component.hpp"
#include "sdk/ui/text_button.hpp"
#include "sdk/ui/theme.hpp"

namespace micropixel::ui {
struct SegmentedStyle final {
    TextButtonStyle normal{kPanelTheme.ButtonStyle()}, selected{kPanelTheme.ButtonStyle(true)};
    int32_t gap{18};
    bool underline{};
    Color mark{kPanelTheme.accent};
};

// Selection is controlled by the caller, including reselect behavior.
template <size_t Capacity>
class SegmentedControl final {
   public:
    static Result<SegmentedControl> Create(Container& parent, Rect bounds, std::span<const char* const> labels,
                                           SegmentedStyle style = {}) {
        if (bounds.empty() || labels.empty() || labels.size() > Capacity || style.gap < 0 ||
            int64_t(bounds.width) - int64_t(labels.size() - 1) * style.gap < int64_t(labels.size()))
            return unexpected(Error{ErrorCode::kInvalidArgument});
        for (const auto* label : labels)
            if (!label || !*label || std::char_traits<char>::length(label) > TextButton::kMaxTextBytes)
                return unexpected(Error{ErrorCode::kInvalidArgument});
        auto root = parent.CreateContainer({.translation = {bounds.x, bounds.y}});
        if (!root) return unexpected(root.error());
        SegmentedControl result;
        result.root_ = ComponentRoot(*root);
        result.bounds_ = bounds;
        result.style_ = style;
        result.count_ = labels.size();
        int32_t available = bounds.width - int32_t(labels.size() - 1) * style.gap;
        for (uint32_t i = 0; i < labels.size(); ++i) {
            const int32_t left = int32_t(int64_t(available) * i / labels.size()) + int32_t(i) * style.gap;
            const int32_t right = int32_t(int64_t(available) * (i + 1) / labels.size()) + int32_t(i) * style.gap;
            result.buttons_[i] = result.root_.node().CreateTextButton(
                {.bounds = {left, 0, right - left, bounds.height}, .text = labels[i], .style = style.normal});
            if (style.underline) {
                auto mark =
                    result.root_.node().CreateShape({(left + right) / 2 - 20, bounds.height - 4, 40, 3}, style.mark);
                if (!mark) return unexpected(mark.error());
                result.marks_[i] = *mark;
            }
        }
        result.SetSelected(0);
        return result;
    }
    bool valid() const { return root_.valid(); }
    void SetSelected(uint32_t selected) {
        if (!valid()) return;
        Assert(selected < count_, "segmented index out of range");
        if (selected_ == selected) return;
        CancelInput();
        selected_ = selected;
        for (uint32_t i = 0; i < count_; ++i) {
            buttons_[i].SetStyle(i == selected ? style_.selected : style_.normal).value();
            if (marks_[i].valid()) marks_[i].SetVisible(i == selected);
        }
    }
    uint32_t selected() const { return selected_; }
    void SetEnabled(uint32_t index, bool enabled) {
        Assert(index < count_, "segmented index out of range");
        if (valid()) buttons_[index].SetEnabled(enabled);
    }
    void CancelInput() {
        if (!valid()) return;
        for (uint32_t i = 0; i < count_; ++i) buttons_[i].Reset();
    }
    void SetVisible(bool visible) {
        if (!valid()) return;
        if (!visible) CancelInput();
        visible_ = visible;
        root_.node().SetVisible(visible);
    }
    UiUpdate<uint32_t> OnTouch(const TouchEvent& touch) {
        if (!valid() || !visible_) return {};
        UiUpdate<uint32_t> result;
        for (uint32_t i = 0; i < count_; ++i) {
            auto update = buttons_[i].OnTouch(touch);
            result.handled |= update.handled;
            result.changed |= update.visual_changed;
            if (update.clicked) result.action = i;
        }
        return result;
    }
    void Destroy() { root_.Destroy(); }

   private:
    ComponentRoot root_;
    std::array<TextButton, Capacity> buttons_;
    std::array<ShapeNode, Capacity> marks_;
    SegmentedStyle style_;
    Rect bounds_;
    uint32_t count_{}, selected_{UINT32_MAX};
    bool visible_{true};
};
}  // namespace micropixel::ui
