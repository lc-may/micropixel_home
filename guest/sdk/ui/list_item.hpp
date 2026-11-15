// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdk/ui/icon_button.hpp"

namespace micropixel::ui {
struct ListItemUpdate {
    bool redraw{}, clicked{}, action_clicked{};
};
struct ListItemStyle {
    const char* action_icon{symbols::kPlus};
    const char* selected_action_icon{symbols::kOk};
    Color background{Color::White()}, title{Color::Rgb(28, 34, 43)}, subtitle{Color::Rgb(120, 128, 140)};
    Color action{Color::Rgb(145, 151, 161)}, selected{Color::Rgb(218, 55, 73)};
};
// Separate body/action hit regions. Labels are clipped before the trailing action.
class ListItem final {
   public:
    ListItem() = default;
    ListItem(const ListItem&) = delete;
    ListItem& operator=(const ListItem&) = delete;
    ListItem(ListItem&&) noexcept = default;
    ListItem& operator=(ListItem&&) noexcept = default;
    bool valid() const { return root_.valid(); }
    static ListItem CreateIn(Container& parent, Rect bounds, ListItemStyle style = {}) {
        Assert(bounds.width >= 160 && bounds.height >= 64, "list item bounds invalid");
        ListItem result;
        result.bounds_ = bounds;
        result.style_ = style;
        result.root_ =
            parent.CreateContainer({.clip = {0, 0, bounds.width, bounds.height}, .translation = {bounds.x, bounds.y}})
                .value();
        result.body_ = Button({0, 0, bounds.width - 64, bounds.height});
        result.feedback_ = result.root_.CreateShape({0, 0, bounds.width, bounds.height}, style.selected, 0).value();
        auto text =
            result.root_.CreateContainer({.clip = {0, 0, bounds.width - 132, bounds.height}, .translation = {52, 0}})
                .value();
        result.title_ = text.CreateLabel({0, 9}, " ", style.title, SystemFont::kMedium).value();
        result.subtitle_ = text.CreateLabel({0, 41}, " ", style.subtitle, SystemFont::kSmall).value();
        result.leading_ = result.root_.CreateLabel({20, 24}, " ", style.subtitle, SystemFont::kSmall, true).value();
        result.divider_ =
            result.root_.CreateShape({52, bounds.height - 1, bounds.width - 64, 1}, Color::Rgb(235, 238, 242)).value();
        result.action_ = IconButton::CreateIn(result.root_, {.bounds = {bounds.width - 60, 8, 56, bounds.height - 16},
                                                             .icon = style.action_icon,
                                                             .selected_icon = style.selected_action_icon,
                                                             .style = {.background = style.background,
                                                                       .text = style.action,
                                                                       .feedback = Color::White(),
                                                                       .font = SystemFont::kLarge,
                                                                       .corner_radius = 20},
                                                             .selected_color = style.selected});
        return result;
    }
    void Destroy() {
        if (root_.valid()) {
            Reset();
            visible_ = false;
            root_.Destroy().value();
        }
    }
    void SetPosition(Point position) {
        bounds_.x = position.x;
        bounds_.y = position.y;
        root_.SetTranslation(position);
    }
    Result<void> SetText(const char* title, const char* subtitle, const char* leading = " ") {
        FixedString<129> t, s;
        FixedString<17> l;
        if (!t.Append(title && *title ? title : " ") || !s.Append(subtitle && *subtitle ? subtitle : " ") ||
            !l.Append(leading && *leading ? leading : " "))
            return unexpected(Error{ErrorCode::kInvalidArgument});
        title_.SetText(t.c_str());
        subtitle_.SetText(s.c_str());
        leading_.SetText(l.c_str());
        return {};
    }
    void SetSelected(bool selected) { title_.SetColor(selected ? style_.selected : style_.title); }
    void SetAction(bool visible, bool selected, bool enabled, bool pending = false) {
        action_visible_ = visible;
        action_.SetVisible(visible);
        action_.SetSelected(selected);
        action_.SetEnabled(enabled);
        action_.SetPending(pending);
        body_.SetBounds({0, 0, bounds_.width - (visible ? 64 : 0), bounds_.height});
    }
    void SetVisible(bool visible) {
        visible_ = visible;
        root_.SetVisible(visible);
        if (!visible) Reset();
    }
    void SetEnabled(bool enabled) {
        enabled_ = enabled;
        body_.SetEnabled(enabled);
        if (!enabled) Reset();
    }
    void Reset() {
        body_.Reset();
        action_.Reset();
        feedback_.SetOpacity(0);
    }
    ListItemUpdate OnTouch(const TouchEvent& event) {
        if (!visible_ || !enabled_) return {};
        auto body = body_.OnTouch(event.WithPosition(root_.ToLocal(event.position())));
        auto action = action_visible_ ? action_.OnTouch(event) : ButtonUpdate{};
        if (body.visual_changed) feedback_.SetOpacity(body_.pressed() ? 16 : 0);
        return {body.redraw() || action.redraw(), body.clicked, action.clicked};
    }

   private:
    ContainerNode root_;
    ShapeNode feedback_, divider_;
    LabelNode title_, subtitle_, leading_;
    IconButton action_;
    Button body_;
    Rect bounds_;
    ListItemStyle style_;
    bool visible_{true}, enabled_{true}, action_visible_{true};
};
}  // namespace micropixel::ui

namespace micropixel {
inline ui::ListItem Container::CreateListItem(Rect bounds) { return ui::ListItem::CreateIn(*this, bounds); }
inline ui::ListItem Container::CreateListItem(Rect bounds, const ui::ListItemStyle& style) {
    return ui::ListItem::CreateIn(*this, bounds, style);
}
}  // namespace micropixel
