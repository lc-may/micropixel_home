// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>

#include "sdk/micropixel.hpp"
namespace micropixel::ui::panel {
inline constexpr Color Background = Color::Rgb(16, 18, 20), Surface = Color::Rgb(27, 31, 35),
                       Raised = Color::Rgb(36, 42, 47), Text = Color::Rgb(245, 247, 248),
                       Muted = Color::Rgb(140, 151, 158), Accent = Color::Rgb(232, 161, 92),
                       Blue = Color::Rgb(127, 182, 217), Red = Color::Rgb(230, 80, 96), Line = Color::Rgb(43, 50, 56),
                       OnAccent = Color::Rgb(26, 19, 5), Disabled = Color::Rgb(108, 117, 123);
inline TextButtonStyle Style(bool selected = false) {
    return {.background = selected ? Accent : Surface,
            .text = selected ? OnAccent : Text,
            .feedback = Muted,
            .font = SystemFont::kMedium,
            .corner_radius = 24};
}
// Visual switch and an independent 84 px touch target.
class Switch {
   public:
    static Switch Create(Container& parent, Rect hit) {
        Switch result;
        result.hit_ = Button(hit);
        result.track_ =
            parent.CreateRoundedRect({hit.x, hit.y + (hit.height - 48) / 2, 84, 48}, {.fill = Raised, .radius = 24})
                .value();
        result.knob_ =
            parent.CreateRoundedRect({hit.x + 5, hit.y + (hit.height - 38) / 2, 38, 38}, {.fill = Muted, .radius = 19})
                .value();
        result.bounds_ = hit;
        return result;
    }
    void Set(bool on) {
        track_.SetFillColor(on ? Accent : Raised);
        knob_.SetFillColor(on ? OnAccent : Muted);
        knob_.SetRect({bounds_.x + (on ? 41 : 5), bounds_.y + (bounds_.height - 38) / 2, 38, 38});
    }
    bool Touch(const TouchEvent& event) { return hit_.OnTouch(event).clicked; }
    void Reset() { hit_.Reset(); }

   private:
    Button hit_;
    Rect bounds_;
    RoundedRectNode track_, knob_;
};
inline void Card(Container& parent, Rect bounds, Color color = Surface) {
    (void)parent.CreateRoundedRect(bounds, {.fill = color, .radius = 24}).value();
}
inline const char* WeatherName(int code) {
    if (code == 0) return "晴";
    if (code == 1 || code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code == 51 || code == 53 || code == 55) return "毛毛雨";
    if (code == 56 || code == 57) return "冻毛毛雨";
    if (code == 61 || code == 63 || code == 65) return "雨";
    if (code == 66 || code == 67) return "冻雨";
    if (code == 71 || code == 73 || code == 75 || code == 77) return "雪";
    if (code == 80 || code == 81 || code == 82) return "阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code == 95 || code == 96 || code == 99) return "雷暴";
    return "未知天气";
}
class WeatherGlyph {
   public:
    static WeatherGlyph Create(Container& parent, Point point, int size = 100) {
        WeatherGlyph glyph;
        glyph.root_ = parent.CreateContainer({.translation = point}).value();
        glyph.sun_ =
            glyph.root_
                .CreateRoundedRect({size / 2, 0, size / 2, size / 2}, {.fill = Accent, .radius = uint32_t(size / 4)})
                .value();
        glyph.cloud_ = glyph.root_.CreateContainer().value();
        (void)glyph.cloud_
            .CreateRoundedRect({0, size * 2 / 5, size * 9 / 10, size * 2 / 5},
                               {.fill = Text, .radius = uint32_t(size / 5)})
            .value();
        (void)glyph.cloud_
            .CreateRoundedRect({size / 6, size / 4, size / 2, size / 2}, {.fill = Text, .radius = uint32_t(size / 4)})
            .value();
        glyph.rain_ = glyph.root_.CreateContainer().value();
        for (unsigned i = 0; i < 3; ++i)
            (void)glyph.rain_
                .CreateRoundedRect({size / 6 + int(i) * size / 4, size * 7 / 8, 4, size / 6},
                                   {.fill = Blue, .radius = 2})
                .value();
        glyph.Set(1, true);
        return glyph;
    }
    void Set(int code, bool known) {
        if (initialized_ && code_ == code && known_ == known) return;
        initialized_ = true;
        code_ = code;
        known_ = known;
        root_.SetOpacity(known ? 255 : 80);
        sun_.SetVisible(known && code <= 2);
        cloud_.SetVisible(!known || code != 0);
        rain_.SetVisible(known && code >= 50);
    }

   private:
    ContainerNode root_, cloud_, rain_;
    RoundedRectNode sun_;
    int code_{};
    bool known_{}, initialized_{};
};
// Retained scalable digits for small displays without extra font assets.
class Digits {
   public:
    static Digits Create(Container& parent, Point at, int height, Color color = Text, uint32_t capacity = 5) {
        Digits result;
        Assert(capacity >= 1 && capacity <= 8 && height >= 24, "panel digit bounds invalid");
        result.capacity_ = capacity;
        result.at_ = at;
        result.height_ = height;
        const int w = height / 2, t = height / 12;
        for (uint32_t i = 0; i < result.capacity_; ++i) {
            result.cells_[i] = parent.CreateContainer().value();
            const int x = 0, y = 0;
            const Rect r[] = {{x + t, y, w - 2 * t, t},
                              {x + w - t, y + t, t, height / 2 - 2 * t},
                              {x + w - t, y + height / 2 + t, t, height / 2 - 2 * t},
                              {x + t, y + height - t, w - 2 * t, t},
                              {x, y + height / 2 + t, t, height / 2 - 2 * t},
                              {x, y + t, t, height / 2 - 2 * t},
                              {x + t, y + height / 2 - t / 2, w - 2 * t, t}};
            for (unsigned j = 0; j < 7; ++j)
                result.parts_[i][j] =
                    result.cells_[i].CreateRoundedRect(r[j], {.fill = color, .radius = uint32_t(t / 2)}).value();
            for (unsigned j = 0; j < 3; ++j)
                result.dots_[i][j] = result.cells_[i]
                                         .CreateRoundedRect({x + (j < 2 ? w / 2 : t),
                                                             y + (j < 2 ? height * int(j + 1) / 3 : height - t), t, t},
                                                            {.fill = color, .radius = uint32_t(t / 2)})
                                         .value();
        }
        result.Set("");
        return result;
    }
    void Set(const char* text) {
        if (initialized_ && std::string_view(last_.c_str()) == text) return;
        initialized_ = true;
        last_.Clear();
        last_.Append(text);
        constexpr uint8_t masks[]{63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
        bool ended = false;
        int advance = 0;
        for (uint32_t i = 0; i < capacity_; ++i) {
            char ch = ended ? '\0' : text[i];
            if (!ch) ended = true;
            cells_[i].SetTranslation({at_.x + advance, at_.y});
            advance += ch == '.' ? height_ / 6 : ch == ':' ? height_ / 3 : height_ * 2 / 3;
            uint8_t mask = ch >= '0' && ch <= '9' ? masks[ch - '0'] : ch == '-' ? 0x40 : 0;
            for (unsigned j = 0; j < 7; ++j) parts_[i][j].SetVisible(mask & (1U << j));
            for (unsigned j = 0; j < 3; ++j) dots_[i][j].SetVisible((ch == ':' && j < 2) || (ch == '.' && j == 2));
        }
    }

   private:
    std::array<std::array<RoundedRectNode, 7>, 8> parts_{};
    std::array<std::array<RoundedRectNode, 3>, 8> dots_{};
    std::array<ContainerNode, 8> cells_{};
    Point at_{};
    int height_{};
    FixedString<9> last_;
    bool initialized_{};
    uint32_t capacity_{};
};
}  // namespace micropixel::ui::panel
