// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string_view>

#include "sdk/symbols.hpp"
#include "sdk/ui/text_button.hpp"

namespace micropixel::ui {
struct IconButtonProperties {
    Rect bounds{};
    const char* icon{symbols::kPlay};
    const char* selected_icon{};
    TextButtonStyle style{};
    Color selected_color{Color::Rgb(218, 55, 73)};
    bool enabled{true};
};
// Selection belongs to the application, so asynchronous actions can wait for acknowledgement.
class IconButton final {
   public:
    IconButton() = default;
    IconButton(const IconButton&) = delete;
    IconButton& operator=(const IconButton&) = delete;
    IconButton(IconButton&&) noexcept = default;
    IconButton& operator=(IconButton&&) noexcept = default;
    bool valid() const { return button_.valid(); }
    static IconButton CreateIn(Container& parent, const IconButtonProperties& properties) {
        IconButton result;
        result.style_ = properties.style;
        result.selected_color_ = properties.selected_color;
        Assert(result.icon_.Append(properties.icon), "icon button text invalid");
        Assert(result.selected_icon_.Append(properties.selected_icon ? properties.selected_icon : properties.icon),
               "selected icon invalid");
        result.button_ = parent.CreateTextButton({.bounds = properties.bounds,
                                                  .text = result.icon_.c_str(),
                                                  .style = properties.style,
                                                  .enabled = properties.enabled});
        result.enabled_ = properties.enabled;
        return result;
    }
    ButtonUpdate OnTouch(const TouchEvent& event) {
        if (!visible_ || pending_) return {};
        return button_.OnTouch(event);
    }
    void SetSelected(bool selected) {
        if (selected_ != selected) {
            selected_ = selected;
            Sync();
        }
    }
    bool selected() const { return selected_; }
    void SetPending(bool pending) {
        if (pending_ != pending) {
            pending_ = pending;
            Sync();
        }
    }
    bool pending() const { return pending_; }
    void SetEnabled(bool enabled) {
        if (enabled_ != enabled) {
            enabled_ = enabled;
            Sync();
        }
    }
    void SetVisible(bool visible) {
        visible_ = visible;
        button_.SetVisible(visible);
    }
    Result<void> SetIcon(const char* icon) {
        if (icon && std::string_view(icon) == icon_.c_str() && std::string_view(icon) == selected_icon_.c_str())
            return {};
        FixedString<17> value;
        if (!icon || !*icon || !value.Append(icon)) return unexpected(Error{ErrorCode::kInvalidArgument});
        icon_ = value;
        selected_icon_ = value;
        Sync();
        return {};
    }
    Result<void> SetBounds(Rect bounds) { return button_.SetBounds(bounds); }
    void Reset() { button_.Reset(); }
    void Destroy() { button_.Destroy(); }

   private:
    void Sync() {
        auto style = style_;
        style.text = selected_ ? selected_color_ : style_.text;
        button_.SetStyle(style).value();
        button_.SetText(pending_ ? symbols::kRefresh : selected_ ? selected_icon_.c_str() : icon_.c_str()).value();
        button_.SetEnabled(enabled_ && !pending_);
    }
    TextButton button_;
    TextButtonStyle style_;
    Color selected_color_{Color::Rgb(218, 55, 73)};
    FixedString<17> icon_, selected_icon_;
    bool enabled_{true}, visible_{true}, selected_{}, pending_{};
};
}  // namespace micropixel::ui

namespace micropixel {
inline ui::IconButton Container::CreateIconButton(const ui::IconButtonProperties& properties) {
    return ui::IconButton::CreateIn(*this, properties);
}
}  // namespace micropixel
