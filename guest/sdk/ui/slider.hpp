// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>

#include "sdk/scene.hpp"
#include "sdk/ui/button.hpp"

namespace micropixel::ui {
struct SliderUpdate {
    bool changed{}, committed{}, cancelled{};
};
struct SliderProperties {
    Rect bounds{};
    uint32_t maximum{100}, value{};
    Color track{Color::Rgb(220, 224, 230)}, fill{Color::Rgb(218, 55, 73)}, thumb{Color::White()};
};
class Slider final {
   public:
    Slider() = default;
    Slider(const Slider&) = delete;
    Slider& operator=(const Slider&) = delete;
    Slider(Slider&&) noexcept = default;
    Slider& operator=(Slider&&) noexcept = default;
    bool valid() const { return root_.valid(); }
    static Slider CreateIn(Container& parent, const SliderProperties& properties) {
        Assert(properties.bounds.width >= 24 && properties.bounds.height >= 20, "slider bounds invalid");
        Slider result;
        result.bounds_ = properties.bounds;
        result.root_ = parent
                           .CreateContainer({.clip = {0, 0, properties.bounds.width, properties.bounds.height},
                                             .translation = {properties.bounds.x, properties.bounds.y}})
                           .value();
        const auto y = properties.bounds.height / 2 - 2;
        result.track_ =
            result.root_
                .CreateRoundedRect({8, y, properties.bounds.width - 16, 4}, {.fill = properties.track, .radius = 2})
                .value();
        result.fill_ = result.root_.CreateRoundedRect({8, y, 1, 4}, {.fill = properties.fill, .radius = 2}).value();
        result.thumb_ =
            result.root_.CreateRoundedRect({0, y - 6, 16, 16}, {.fill = properties.thumb, .radius = 8}).value();
        result.maximum_ = properties.maximum;
        result.value_ = std::min(properties.value, properties.maximum);
        result.Sync();
        return result;
    }
    void Destroy() {
        if (root_.valid()) {
            Cancel();
            visible_ = false;
            root_.Destroy().value();
        }
    }
    void SetMaximum(uint32_t maximum) {
        if (maximum_ == maximum) return;
        Cancel();
        maximum_ = maximum;
        value_ = std::min(value_, maximum_);
        Sync();
    }
    void SetValue(uint32_t value) {
        if (!dragging_) {
            const auto next = std::min(value, maximum_);
            if (value_ != next) {
                value_ = next;
                Sync();
            }
        }
    }
    uint32_t value() const { return value_; }
    bool dragging() const { return dragging_; }
    void SetEnabled(bool enabled) {
        enabled_ = enabled;
        if (!enabled) Cancel();
    }
    void SetVisible(bool visible) {
        visible_ = visible;
        if (!visible) Cancel();
        root_.SetVisible(visible);
    }
    void Cancel() {
        if (dragging_) {
            value_ = anchor_;
            dragging_ = false;
            Sync();
        }
    }
    SliderUpdate OnTouch(const TouchEvent& event) {
        if (!visible_ || !enabled_ || !maximum_) return {};
        const auto point = root_.ToLocal(event.position());
        if (event.phase() == TouchPhase::kDown) {
            if (dragging_ || !Rect{0, 0, bounds_.width, bounds_.height}.contains(point)) return {};
            dragging_ = true;
            touch_id_ = event.id();
            anchor_ = value_;
        } else if (!dragging_ || event.id() != touch_id_)
            return {};
        if (event.phase() == TouchPhase::kCancel) {
            const bool changed = value_ != anchor_;
            Cancel();
            return {changed, false, true};
        }
        const auto x = std::clamp<int64_t>(int64_t(point.x) - 8, 0, bounds_.width - 16);
        const auto next = uint32_t(uint64_t(x) * maximum_ / uint32_t(bounds_.width - 16));
        const bool changed = value_ != next;
        value_ = next;
        const bool committed = event.phase() == TouchPhase::kUp;
        if (committed) dragging_ = false;
        if (changed) Sync();
        return {changed, committed, false};
    }

   private:
    void Sync() {
        auto x = maximum_ ? int32_t(uint64_t(value_) * uint32_t(bounds_.width - 16) / maximum_) : 0;
        const auto y = bounds_.height / 2 - 2;
        fill_.SetRect({8, y, std::max(1, x), 4});
        thumb_.SetRect({x, y - 6, 16, 16});
    }
    ContainerNode root_;
    RoundedRectNode track_, fill_, thumb_;
    Rect bounds_;
    uint32_t maximum_{}, value_{}, anchor_{}, touch_id_{};
    bool enabled_{true}, visible_{true}, dragging_{};
};
}  // namespace micropixel::ui

namespace micropixel {
inline ui::Slider Container::CreateSlider(const ui::SliderProperties& properties) {
    return ui::Slider::CreateIn(*this, properties);
}
}  // namespace micropixel
