// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>

#include "sdk/geometry.hpp"

namespace netease {

struct MusicLayout final {
    int32_t width{720};
    int32_t height{720};

    static MusicLayout ForDisplay(micropixel::Size display) {
        if (std::min(display.width, display.height) > 480U) return {};
        const auto content_height = uint64_t(display.height) * 480U / display.width;
        return {480, static_cast<int32_t>(std::clamp<uint64_t>(content_height, 436, 480))};
    }

    bool compact() const { return width == 480; }
    int32_t center() const { return width / 2; }
    int32_t mini_top() const { return compact() ? height - 64 : 650; }
    micropixel::Rect box(micropixel::Rect desktop, micropixel::Rect small) const { return compact() ? small : desktop; }
    micropixel::Point point(micropixel::Point desktop, micropixel::Point small) const {
        return compact() ? small : desktop;
    }
    micropixel::Rect mini_cover() const { return box({32, mini_top() + 10, 42, 42}, {20, mini_top() + 9, 38, 38}); }
    micropixel::Rect player_cover() const { return box({48, 112, 104, 104}, {18, 68, 80, 80}); }
};

}  // namespace netease
