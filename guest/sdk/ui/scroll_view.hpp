// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>

#include "sdk/graphics.hpp"
#include "sdk/panic.hpp"

namespace micropixel::ui {
struct ScrollUpdate {
    bool changed{}, dragging{};
};
// Touch scrolling with a movement threshold; the app recycles its visible row pool.
class ScrollView final {
   public:
    ScrollView() = default;
    ScrollView(const ScrollView&) = delete;
    ScrollView& operator=(const ScrollView&) = delete;
    ScrollView(ScrollView&&) noexcept = default;
    ScrollView& operator=(ScrollView&&) noexcept = default;
    bool valid() const { return root_.valid(); }
    static ScrollView CreateIn(Container& parent, Rect bounds) {
        Assert(!bounds.empty(), "scroll bounds invalid");
        ScrollView result;
        result.bounds_ = bounds;
        result.root_ =
            parent.CreateContainer({.clip = {0, 0, bounds.width, bounds.height}, .translation = {bounds.x, bounds.y}})
                .value();
        result.content_ = result.root_.CreateContainer().value();
        result.bar_ =
            result.root_
                .CreateRoundedRect({bounds.width - 3, 0, 3, 24}, {.fill = Color::Rgb(203, 207, 214), .radius = 1})
                .value();
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
    Container& content() { return content_; }
    int32_t offset() const { return offset_; }
    bool at_end(int32_t threshold = 0) const {
        return int64_t(offset_) + bounds_.height + std::max(0, threshold) >= height_;
    }
    Rect bounds() const { return bounds_; }
    bool contains(Point scene) const {
        return Rect{0, 0, bounds_.width, bounds_.height}.contains(root_.ToLocal(scene));
    }
    void SetBounds(Rect bounds) {
        Assert(!bounds.empty(), "scroll bounds invalid");
        if (bounds == bounds_) return;
        Cancel();
        bounds_ = bounds;
        root_.SetClip({0, 0, bounds.width, bounds.height});
        root_.SetTranslation({bounds.x, bounds.y});
        ScrollTo(offset_);
    }
    void SetContentHeight(int32_t height) {
        height_ = std::max(0, height);
        ScrollTo(offset_);
    }
    void ScrollTo(int32_t offset) {
        offset_ = std::clamp(offset, 0, std::max(0, height_ - bounds_.height));
        Sync();
    }
    void Cancel() {
        tracking_ = false;
        dragging_ = false;
    }
    void SetVisible(bool visible) {
        visible_ = visible;
        if (!visible) Cancel();
        root_.SetVisible(visible);
    }
    ScrollUpdate OnTouch(const TouchEvent& event) {
        if (!visible_) return {};
        if (event.phase() == TouchPhase::kDown) {
            if (tracking_ || !contains(event.position())) return {};
            tracking_ = true;
            touch_id_ = event.id();
            start_ = event.position();
            anchor_ = offset_;
            return {};
        }
        if (!tracking_ || event.id() != touch_id_) return {};
        const int64_t dy = int64_t(event.y()) - start_.y, dx = int64_t(event.x()) - start_.x;
        if (event.phase() == TouchPhase::kMove && (dy > 8 || dy < -8 || dx > 8 || dx < -8)) dragging_ = true;
        const bool was_dragging = dragging_;
        const auto old = offset_;
        if (dragging_ && event.phase() != TouchPhase::kCancel) {
            auto target = std::clamp<int64_t>(int64_t(anchor_) - dy, 0, std::max(0, height_ - bounds_.height));
            ScrollTo(int32_t(target));
        }
        if (event.phase() == TouchPhase::kUp || event.phase() == TouchPhase::kCancel) Cancel();
        return {old != offset_, was_dragging};
    }

   private:
    void Sync() {
        content_.SetTranslation({0, -offset_});
        bar_.SetVisible(height_ > bounds_.height);
        const int32_t h = std::min(
            bounds_.height, std::max(24, int32_t(int64_t(bounds_.height) * bounds_.height / std::max(1, height_))));
        const auto y = height_ > bounds_.height
                           ? int32_t(int64_t(offset_) * (bounds_.height - h) / (height_ - bounds_.height))
                           : 0;
        bar_.SetRect({bounds_.width - 3, y, 3, h});
    }
    ContainerNode root_, content_;
    RoundedRectNode bar_;
    Rect bounds_;
    Point start_;
    int32_t height_{}, offset_{}, anchor_{};
    uint32_t touch_id_{};
    bool visible_{true}, tracking_{}, dragging_{};
};
}  // namespace micropixel::ui

namespace micropixel {
inline ui::ScrollView Container::CreateScrollView(Rect bounds) { return ui::ScrollView::CreateIn(*this, bounds); }
}  // namespace micropixel
