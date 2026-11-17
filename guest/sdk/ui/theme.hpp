// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdk/graphics.hpp"
#include "sdk/scene.hpp"
#include "sdk/ui/text_button.hpp"

namespace micropixel::ui {
struct Theme final {
    Color background{Color::Rgb(16, 18, 20)}, surface{Color::Rgb(27, 31, 35)}, raised{Color::Rgb(36, 42, 47)};
    Color text{Color::Rgb(245, 247, 248)}, muted{Color::Rgb(140, 151, 158)}, accent{Color::Rgb(232, 161, 92)};
    Color on_accent{Color::Rgb(26, 19, 5)}, line{Color::Rgb(43, 50, 56)}, disabled{Color::Rgb(108, 117, 123)};
    int32_t inset{24}, gap{18}, touch_extent{84};
    uint32_t radius{24};
    TextButtonStyle ButtonStyle(bool selected = false) const {
        return {.background = selected ? accent : surface,
                .text = selected ? on_accent : text,
                .feedback = muted,
                .font = SystemFont::kMedium,
                .corner_radius = radius};
    }
};
inline constexpr Theme kPanelTheme{};
}  // namespace micropixel::ui
