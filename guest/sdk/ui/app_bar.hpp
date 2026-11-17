// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <span>

#include "sdk/ui/component.hpp"
#include "sdk/ui/material_icons.hpp"
#include "sdk/ui/theme.hpp"

namespace micropixel::ui {
struct AppBarAction final {
    const char* text{};
    std::optional<MaterialSymbol> icon{};
    int32_t width{84};
};
struct AppBarProperties final {
    Rect bounds{0, 0, 720, 84};
    const char* title{" "};
    Theme theme{};
    std::optional<AppBarAction> leading{};
    std::span<const AppBarAction> actions{};
    int32_t padding{24}, gap{12}, action_height{84}, subtitle_width{};
    int32_t title_line_height{42}, subtitle_line_height{38};
    SystemFont title_font{SystemFont::kLarge}, subtitle_font{SystemFont::kMedium};
    bool title_action{}, surface{};
};
enum class AppBarActionKind { kLeading, kTitle, kAction };
struct AppBarEvent final {
    AppBarActionKind kind{};
    uint32_t index{};
};

class AppBar final {
   public:
    static Result<AppBar> Create(Container& parent, const AppBarProperties& properties,
                                 const MaterialAtlas* atlas = nullptr) {
        if (properties.bounds.empty() || properties.actions.size() > 2 || properties.padding < 0 ||
            properties.gap < 0 || properties.subtitle_width < 0 || properties.action_height <= 0 ||
            properties.action_height > properties.bounds.height || properties.title_line_height <= 0 ||
            properties.subtitle_line_height <= 0 || !ValidText(properties.title))
            return unexpected(Error{ErrorCode::kInvalidArgument});
        for (const auto& action : properties.actions)
            if (action.width <= 0 || (action.icon ? atlas == nullptr : !ValidText(action.text) || !*action.text))
                return unexpected(Error{ErrorCode::kInvalidArgument});
        if (properties.leading &&
            (properties.leading->width <= 0 ||
             (properties.leading->icon ? atlas == nullptr
                                       : !ValidText(properties.leading->text) || !*properties.leading->text)))
            return unexpected(Error{ErrorCode::kInvalidArgument});
        auto root = parent.CreateContainer({.translation = {properties.bounds.x, properties.bounds.y}});
        if (!root) return unexpected(root.error());
        AppBar result;
        result.root_ = ComponentRoot(*root);
        result.properties_ = properties;
        result.properties_.actions = {};
        result.properties_.title = nullptr;
        result.count_ = properties.actions.size();
        result.leading_visible_ = properties.leading.has_value();
        result.action_visible_.fill(true);
        if (properties.surface) {
            auto background = result.root_.node().CreateRoundedRect(
                {0, 0, properties.bounds.width, properties.bounds.height},
                {.fill = properties.theme.surface, .radius = properties.theme.radius});
            if (!background) return unexpected(background.error());
            result.background_ = *background;
        }
        auto clip = result.root_.node().CreateContainer();
        if (!clip) return unexpected(clip.error());
        result.title_clip_ = *clip;
        auto title = result.title_clip_.CreateLabel({0, 0}, *properties.title ? properties.title : " ",
                                                    properties.theme.text, properties.title_font);
        if (!title) return unexpected(title.error());
        result.title_ = *title;
        auto subtitle_clip = result.root_.node().CreateContainer();
        if (!subtitle_clip) return unexpected(subtitle_clip.error());
        result.subtitle_clip_ = *subtitle_clip;
        auto subtitle =
            result.subtitle_clip_.CreateLabel({0, 0}, " ", properties.theme.muted, properties.subtitle_font);
        if (!subtitle) return unexpected(subtitle.error());
        result.subtitle_ = *subtitle;
        for (uint32_t i = 0; i < result.count_; ++i) result.CreateAction(i, properties.actions[i], atlas);
        if (properties.leading) result.CreateAction(2, *properties.leading, atlas);
        auto layout = result.Layout(properties.bounds);
        if (!layout) return unexpected(layout.error());
        return result;
    }
    bool valid() const { return root_.valid(); }
    Result<void> SetTitle(const char* text) {
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        if (!ValidText(text)) return unexpected(Error{ErrorCode::kInvalidArgument});
        title_.SetText(*text ? text : " ");
        return {};
    }
    Result<void> SetSubtitle(const char* text) {
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        if (!ValidText(text)) return unexpected(Error{ErrorCode::kInvalidArgument});
        subtitle_.SetText(*text ? text : " ");
        return {};
    }
    Result<void> SetBounds(Rect bounds) { return Layout(bounds); }
    void SetLeadingVisible(bool visible) {
        if (!valid() || !properties_.leading || leading_visible_ == visible) return;
        CancelInput();
        leading_visible_ = visible;
        ShowAction(2, visible);
        Layout(properties_.bounds).value();
    }
    void SetActionVisible(uint32_t index, bool visible) {
        Assert(index < count_, "app bar action index");
        if (!valid() || action_visible_[index] == visible) return;
        CancelInput();
        action_visible_[index] = visible;
        ShowAction(index, visible);
        Layout(properties_.bounds).value();
    }
    void SetActionEnabled(uint32_t index, bool enabled) {
        Assert(index < count_, "app bar action index");
        if (!valid()) return;
        if (material_[index])
            icons_[index].SetEnabled(enabled);
        else
            buttons_[index].SetEnabled(enabled);
    }
    Result<void> SetActionText(uint32_t index, const char* text) {
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        if (index >= count_ || material_[index]) return unexpected(Error{ErrorCode::kInvalidArgument});
        return buttons_[index].SetText(text);
    }
    void CancelInput() {
        title_hit_.Reset();
        if (!valid()) return;
        for (uint32_t i = 0; i < 3; ++i) {
            icons_[i].Reset();
            if (buttons_[i].valid()) buttons_[i].Reset();
        }
    }
    void SetVisible(bool visible) {
        if (!valid()) return;
        if (!visible) CancelInput();
        visible_ = visible;
        root_.node().SetVisible(visible);
    }
    UiUpdate<AppBarEvent> OnTouch(const TouchEvent& event) {
        if (!valid() || !visible_) return {};
        UiUpdate<AppBarEvent> result;
        for (uint32_t i = 0; i < 3; ++i) {
            if (i == 2 ? !leading_visible_ : i >= count_ || !action_visible_[i]) continue;
            auto update = material_[i] ? icons_[i].OnTouch(event) : buttons_[i].OnTouch(event);
            result.handled |= update.handled;
            result.changed |= update.visual_changed;
            if (update.clicked)
                result.action = AppBarEvent{i == 2 ? AppBarActionKind::kLeading : AppBarActionKind::kAction, i};
        }
        if (properties_.title_action && !result.handled) {
            auto update = title_hit_.OnTouch(event.WithPosition(root_.node().ToLocal(event.position())));
            result.handled |= update.handled;
            if (update.clicked) result.action = AppBarEvent{AppBarActionKind::kTitle};
        }
        return result;
    }
    void Destroy() { root_.Destroy(); }

   private:
    static bool ValidText(const char* text) {
        FixedString<TextButton::kMaxTextBytes + 1> copy;
        return text && copy.Append(text);
    }
    void CreateAction(uint32_t index, const AppBarAction& action, const MaterialAtlas* atlas) {
        widths_[index] = action.width;
        material_[index] = action.icon.has_value();
        Rect bounds{0, 0, action.width, properties_.action_height};
        if (action.icon)
            icons_[index] =
                MaterialIconButton::Create(root_.node(), *atlas, bounds, *action.icon, properties_.theme.background);
        else {
            auto style = properties_.theme.ButtonStyle();
            style.corner_radius = properties_.theme.radius;
            buttons_[index] = root_.node().CreateTextButton({.bounds = bounds, .text = action.text, .style = style});
        }
    }
    void ShowAction(uint32_t index, bool visible) {
        if (material_[index])
            icons_[index].SetVisible(visible);
        else
            buttons_[index].SetVisible(visible);
    }
    Result<void> Layout(Rect bounds) {
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        if (bounds.empty() || bounds.height < properties_.action_height ||
            bounds.height < properties_.title_line_height || bounds.height < properties_.subtitle_line_height)
            return unexpected(Error{ErrorCode::kInvalidArgument});
        int64_t required =
            properties_.padding + (properties_.leading ? int64_t(widths_[2]) + properties_.gap : properties_.padding);
        required += properties_.subtitle_width ? int64_t(properties_.subtitle_width) + properties_.gap : 0;
        for (uint32_t i = 0; i < count_; ++i) required += int64_t(widths_[i]) + properties_.gap;
        if (required >= bounds.width) return unexpected(Error{ErrorCode::kInvalidArgument});
        std::array<Rect, 3> positions{};
        int64_t right = int64_t(bounds.width) - properties_.padding;
        for (uint32_t n = count_; n > 0; --n) {
            const auto i = n - 1;
            if (!action_visible_[i]) continue;
            right -= widths_[i];
            if (right < 0) return unexpected(Error{ErrorCode::kInvalidArgument});
            positions[i] = {int32_t(right), (bounds.height - properties_.action_height) / 2, widths_[i],
                            properties_.action_height};
            right -= properties_.gap;
        }
        const int64_t left = leading_visible_ ? int64_t(widths_[2]) + properties_.gap : properties_.padding;
        const int64_t subtitle_left = right - properties_.subtitle_width;
        const int64_t title_right = properties_.subtitle_width ? subtitle_left - properties_.gap : right;
        if (left < 0 || title_right <= left || title_right > INT32_MAX)
            return unexpected(Error{ErrorCode::kInvalidArgument});
        CancelInput();
        properties_.bounds = bounds;
        root_.node().SetTranslation({bounds.x, bounds.y});
        title_clip_.SetTranslation({int32_t(left), 0});
        title_clip_.SetClip({0, 0, int32_t(title_right - left), bounds.height});
        title_.SetPosition({0, (bounds.height - properties_.title_line_height) / 2});
        subtitle_clip_.SetTranslation({int32_t(subtitle_left), 0});
        subtitle_clip_.SetClip(properties_.subtitle_width ? Rect{0, 0, properties_.subtitle_width, bounds.height}
                                                          : Rect{});
        subtitle_clip_.SetVisible(properties_.subtitle_width != 0);
        subtitle_.SetPosition({0, (bounds.height - properties_.subtitle_line_height) / 2});
        title_hit_.SetBounds({int32_t(left), 0, int32_t(title_right - left), bounds.height});
        if (background_.valid()) background_.SetRect({0, 0, bounds.width, bounds.height});
        positions[2] = {0, (bounds.height - properties_.action_height) / 2, widths_[2], properties_.action_height};
        for (uint32_t i = 0; i < 3; ++i) {
            if (positions[i].empty()) continue;
            if (material_[i])
                icons_[i].SetBounds(positions[i]).value();
            else if (buttons_[i].valid())
                buttons_[i].SetBounds(positions[i]).value();
        }
        return {};
    }
    ComponentRoot root_;
    ContainerNode title_clip_, subtitle_clip_;
    LabelNode title_, subtitle_;
    RoundedRectNode background_;
    Button title_hit_;
    std::array<MaterialIconButton, 3> icons_;
    std::array<TextButton, 3> buttons_;
    std::array<int32_t, 3> widths_{};
    std::array<bool, 3> material_{}, action_visible_{};
    AppBarProperties properties_;
    uint32_t count_{};
    bool leading_visible_{}, visible_{true};
};
}  // namespace micropixel::ui
