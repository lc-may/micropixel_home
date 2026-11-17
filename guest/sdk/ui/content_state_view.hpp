// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdk/fixed_string.hpp"
#include "sdk/ui/component.hpp"
#include "sdk/ui/theme.hpp"

namespace micropixel::ui {
enum class ContentPhase { kIdle, kLoading, kReady, kFailed };
struct ContentState final {
    ContentPhase phase{ContentPhase::kIdle};
    bool has_content{};
    const char* message{};
    const char* detail{};
};
// A bounded status surface. Cached content remains owned and visible in the App.
class ContentStateView final {
   public:
    static Result<ContentStateView> Create(Container& parent, Rect bounds, Theme theme = {},
                                           SystemFont font = SystemFont::kMedium, bool centered = true,
                                           int32_t detail_offset = 45) {
        if (bounds.empty() || detail_offset < 0) return unexpected(Error{ErrorCode::kInvalidArgument});
        auto root =
            parent.CreateContainer({.clip = {0, 0, bounds.width, bounds.height}, .translation = {bounds.x, bounds.y}});
        if (!root) return unexpected(root.error());
        ContentStateView result;
        result.root_ = ComponentRoot(*root);
        auto label =
            result.root_.node().CreateLabel({centered ? bounds.width / 2 : 0, 0}, " ", theme.muted, font, centered);
        if (!label) return unexpected(label.error());
        result.label_ = *label;
        auto detail = result.root_.node().CreateLabel({centered ? bounds.width / 2 : 0, detail_offset}, " ",
                                                      theme.muted, SystemFont::kMedium, centered);
        if (!detail) return unexpected(detail.error());
        result.detail_ = *detail;
        return result;
    }
    // Text is copied into Scene; invalid updates leave the previous presentation intact.
    Result<void> Set(const ContentState& state) {
        if (!root_.valid()) return unexpected(Error{ErrorCode::kInvalidState});
        FixedString<257> message, detail;
        const char* fallback = state.phase == ContentPhase::kLoading  ? "正在加载…"
                               : state.phase == ContentPhase::kFailed ? "加载失败"
                               : state.phase == ContentPhase::kReady  ? "暂无内容"
                                                                      : " ";
        if (!message.Append(state.message && *state.message ? state.message : fallback) ||
            !detail.Append(state.detail && *state.detail ? state.detail : " "))
            return unexpected(Error{ErrorCode::kInvalidArgument});
        visible_ = !state.has_content;
        root_.node().SetVisible(visible_);
        if (visible_) {
            label_.SetText(message.c_str());
            detail_.SetText(detail.c_str());
        }
        return {};
    }
    bool visible() const { return root_.valid() && visible_; }
    void SetVisible(bool visible) {
        if (!root_.valid()) return;
        visible_ = visible;
        root_.node().SetVisible(visible);
    }
    void Destroy() { root_.Destroy(); }

   private:
    ComponentRoot root_;
    LabelNode label_, detail_;
    bool visible_{true};
};
}  // namespace micropixel::ui
