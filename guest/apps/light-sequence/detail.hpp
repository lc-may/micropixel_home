// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>

#include "art.hpp"

namespace light_sequence {
enum class Field { kLevel, kKelvin, kHue, kSaturation };

inline constexpr std::array kFields{&LightState::level, &LightState::kelvin, &LightState::hue, &LightState::saturation};

inline int& Value(LightState& state, Field field) { return state.*kFields[static_cast<size_t>(field)]; }

inline int Value(const LightState& state, Field field) { return state.*kFields[static_cast<size_t>(field)]; }

class Range {
   public:
    struct Update {
        bool handled{}, commit{};
        int value{};
    };

    Range(const Art& art, Container& root, Field field, Rect bounds, int minimum, int maximum, int step, bool enabled)
        : field_(field),
          bounds_(bounds),
          minimum_(minimum),
          maximum_(maximum),
          step_(step),
          readout_(art.Label(root, field == Field::kLevel ? Text::kBrightnessRead : Text::kSettingRead, 7)) {
        const int title_y = bounds.y - (field == Field::kLevel ? 20 : 21);
        constexpr std::array labels{"亮度", "色温", "色相", "饱和度"};
        const bool icon = field == Field::kLevel || field == Field::kKelvin;
        if (icon)
            art.Glyph(root, field == Field::kLevel ? Icon::kSun : Icon::kTemp, Tone::kControl, 16,
                      {bounds.x, title_y + 2});
        art.Write(root, Text::kControlLabel, {bounds.x + (icon ? 22 : 0), title_y}, labels[static_cast<size_t>(field)]);

        auto visual = root.CreateContainer({.opacity = static_cast<uint8_t>(enabled ? 255 : 128)}).value();
        const Rect track{bounds.x, bounds.center_y() - 5, bounds.width, 11};
        if (field == Field::kLevel) {
            (void)Box(visual, track, Rgb(0x43454e), 6);
            fill_ = visual.CreateContainer().value();
            (void)Box(fill_, track, Rgb(0xf0be82), 6);
        } else if (field == Field::kSaturation) {
            saturation_ = Box(visual, track, Color::Black(), 6);
            art.Draw(visual, Surface::kSaturationCover, {track.x, track.y});
        } else {
            art.Draw(visual, field == Field::kKelvin ? Surface::kTemperatureTrack : Surface::kHueTrack,
                     {track.x, track.y});
        }

        thumb_shadow_ =
            Box(visual, {bounds.x - 1, bounds.center_y() - 11, 26, 26}, Color::Black(), 13, Color::Black(), 0, 85);
        thumb_ = Box(visual, {bounds.x, bounds.center_y() - 12, 24, 24}, Rgb(0xfff0d9), 12, Rgb(0xfff8ec), 2);

        // Keep SDK capture, cancellation and seek handling. Its 8 px track
        // inset plus the extra 4 px matches the reference's 24 px thumb.
        auto gesture = root.CreateContainer({.visible = false}).value();
        slider_ = gesture.CreateSlider(
            {.bounds = {bounds.x + 4, bounds.y, bounds.width - 8, bounds.height}, .maximum = kSliderSteps});
        slider_.SetEnabled(enabled);
    }

    Field field() const { return field_; }
    bool dragging() const { return slider_.dragging(); }

    void Show(int value, Color tint, Color hue) {
        const int extent = maximum_ - minimum_;
        slider_.SetValue((value - minimum_) * kSliderSteps / extent);
        const int track_x = ((value - minimum_) * (bounds_.width - 24) + extent / 2) / extent;
        const int x = bounds_.x + track_x;
        thumb_.SetRect({x, bounds_.center_y() - 12, 24, 24});
        thumb_.SetFillColor(tint);
        if (saturation_.valid()) saturation_.SetFillColor(hue);
        thumb_shadow_.SetRect({x - 1, bounds_.center_y() - 11, 26, 26});
        if (fill_.valid()) {
            const int width = ((value - minimum_) * bounds_.width + extent / 2) / extent;
            fill_.SetVisible(width > 0);
            if (width > 0) fill_.SetClip({bounds_.x, bounds_.center_y() - 5, width, 11});
        }
        constexpr std::array units{"%", " K", "°", "%"};
        Copy text;
        text.AppendInt(value);
        text.Append(units[static_cast<size_t>(field_)]);
        const bool level = field_ == Field::kLevel;
        readout_.Set(level ? Text::kBrightnessRead : Text::kSettingRead,
                     {bounds_.x + bounds_.width, bounds_.y - (level ? 20 : 21)}, text.c_str(), Align::kRight);
    }

    Update Touch(const mp::TouchEvent& event) {
        const bool was_dragging = slider_.dragging();
        if (!was_dragging && !bounds_.contains(event.position())) return {};
        const Point adjusted{std::clamp(event.x(), bounds_.x + 4, bounds_.x + bounds_.width - 5), event.y()};
        const auto update = slider_.OnTouch(event.WithPosition(adjusted));
        return {was_dragging || slider_.dragging(), update.committed, ValueAtThumb()};
    }

    void Cancel(LightState& state) {
        if (!slider_.dragging()) return;
        slider_.Cancel();
        Value(state, field_) = ValueAtThumb();
    }

   private:
    int ValueAtThumb() const {
        const int steps = (maximum_ - minimum_) / step_;
        const int index = (static_cast<int>(slider_.value()) * steps + kSliderSteps / 2) / kSliderSteps;
        return minimum_ + index * step_;
    }

    static constexpr int kSliderSteps = 4096;
    Field field_;
    Rect bounds_;
    int minimum_, maximum_, step_;
    ArtLabel readout_;
    mp::RoundedRectNode thumb_, thumb_shadow_, saturation_;
    mp::ContainerNode fill_;
    ui::Slider slider_;
};

class LightDetail {
   public:
    LightDetail(const Art& art, Container& root, const Light& light, const LightState& state)
        : art_(art), light_(light), lit_(light.online && state.on), rgb_(state.rgb) {
        (void)Box(root, {20, 86, 440, 86}, Rgb(0x242529), 17, kEdge, 1);
        if (lit_) {
            ambient_ = Box(root, {21, 87, 438, 84}, Color::Black(), 16);
            art_.Draw(root, Surface::kPreviewCover, {21, 87});
        }
        (void)Box(root, {34, 95, 69, 69}, Color::White(), 35, Color::White(), 0, lit_ ? 12 : 8);
        orb_ = Box(root, {39, 100, 59, 59}, Rgb(0x2d3037), 30, Rgb(0x56585d), lit_ ? 0 : 1);
        if (lit_) art_.Draw(root, Surface::kOrbCover, {39, 100});
        art_.Glyph(root, FixtureIcon(light.fixture), lit_ ? Tone::kDark : Tone::kOrbOff, 24, {57, 117}, 191);
        art_.Write(root, Text::kPreviewStatus, {115, lit_ ? 103 : 106},
                   !light.online ? "连接恢复后可控制"
                   : lit_        ? "灯光预览"
                                 : "开灯恢复上次设置");
        live_ = art_.Label(root, lit_ ? Text::kPreviewValue : Text::kPreviewOff, 10);
        (void)Box(root, {387, 115, 58, 27}, Color::White(), 14, Color::White(), 1, 16);
        art_.Glyph(root, lit_ ? (rgb_ ? Icon::kColor : Icon::kSun) : (light.online ? Icon::kMoon : Icon::kOffline),
                   Tone::kTag, 13, {396, 122});
        art_.Write(root, Text::kPreviewTag, {414, 121},
                   lit_ ? (rgb_                     ? "彩光"
                           : light.HasTemperature() ? "白光"
                                                    : "已开")
                        : (light.online ? "已关" : "离线"));
        if (light.HasLevel()) {
            (void)Box(root, {20, 182, 440, 75}, Rgb(0x202227), 16, kEdge, 1);
            ranges_[0].emplace(art_, root, Field::kLevel, Rect{34, 209, 412, 46}, 1, 100, 1, lit_);
        }
    }

    template <class AddTemp, class AddSwatch>
    void Settings(Container& root, AddTemp add_temp, AddSwatch add_swatch) {
        if (!light_.HasTemperature()) {
            const int y = light_.HasLevel() ? 267 : 182;
            (void)Box(root, {20, y, 440, 44}, kBackground, 13, Rgb(0x2d2f35), 1);
            art_.Glyph(root, Icon::kInfo, Tone::kInfo, 18, {34, y + 13});
            art_.Write(root, Text::kCapability, {62, y + 13},
                       light_.HasLevel() ? "此灯仅支持开关与亮度" : "此灯仅支持开关");
            return;
        }

        const int y = light_.HasColor() ? 323 : 266;
        (void)Box(root, {20, y, 440, rgb_ ? 141 : 139}, Rgb(0x202227), 16, kEdge, 1);
        if (rgb_) {
            ranges_[1].emplace(art_, root, Field::kHue, Rect{34, y + 32, 198, 46}, 0, 359, 1, lit_);
            ranges_[2].emplace(art_, root, Field::kSaturation, Rect{248, y + 32, 198, 46}, 0, 100, 1, lit_);
            Line(root, {34, 402, 412, 1}, Rgb(0x34363e));
            constexpr std::array x{34, 118, 201, 285, 368};
            for (size_t i = 0; i < kSwatches.size(); ++i) {
                auto group = root.CreateContainer({.opacity = static_cast<uint8_t>(lit_ ? 255 : 128)}).value();
                const auto color = HueColor(kSwatches[i].hue, kSwatches[i].saturation);
                (void)Box(group, {x[i] + 26, 417, 26, 26}, color, 13, Rgb(0xffffff), 1);
                swatch_selection_[i] = group.CreateContainer().value();
                auto& selected = swatch_selection_[i];
                (void)Box(selected, {x[i] + 21, 412, 36, 36}, Rgb(0xffebd3), 18);
                (void)Box(selected, {x[i] + 23, 414, 32, 32}, Rgb(0x202227), 16);
                (void)Box(selected, {x[i] + 26, 417, 26, 26}, color, 13, color.Lightened(85), 1);
                art_.Glyph(selected, Icon::kCheck, Tone::kCheck, 15, {x[i] + 32, 423});
                if (lit_) add_swatch({x[i], 406, 78, 48}, static_cast<int>(i));
            }
        } else {
            ranges_[1].emplace(art_, root, Field::kKelvin, Rect{34, y + 32, 412, 46}, light_.minimum, light_.maximum,
                               100, lit_);
            constexpr std::array x{34, 174, 313};
            const auto temperatures = Temperatures(light_);
            for (size_t i = 0; i < temperatures.size(); ++i) {
                const Rect bounds{x[i], y + 81, 133, 48};
                auto group = root.CreateContainer({.opacity = static_cast<uint8_t>(lit_ ? 255 : 128)}).value();
                auto box = Box(group, bounds, Rgb(0x282a31), 10, Rgb(0x3d3f46), 1);
                Copy text;
                text.AppendInt(temperatures[i]);
                text.Append(" K");
                const int text_width = 26 + 6 + ArtLabel::Width(Text::kKelvin, text.c_str());
                const int left = bounds.center_x() - text_width / 2;
                presets_[i].emplace(box, art_.Label(group, Text::kPreset, 2), Point{left, bounds.y + 15});
                art_.Write(group, Text::kKelvin, {left + 32, bounds.y + 17}, text.c_str());
                if (lit_) add_temp(bounds, temperatures[i]);
            }
        }
    }

    void Show(const LightState& state) {
        const Color color = LightColor(light_, state);
        if (lit_) {
            orb_.SetFillColor(Mix(Rgb(0x312723), color, 0.50F + state.level / 200.0F));
            ambient_.SetFillColor(color);
            const float opacity = (0.23F + state.level / 250.0F) * 1.5F;
            ambient_.SetOpacity(static_cast<uint8_t>(mp::math::RoundToInt(opacity * 255)));
        }
        const auto text = PreviewValue(light_, state, color);
        live_->Set(lit_ ? Text::kPreviewValue : Text::kPreviewOff, {115, lit_ ? 122 : 125}, text.c_str());
        const auto hue = HueColor(state.hue, 100);
        for (auto& range : ranges_) {
            if (!range) continue;
            const Field field = range->field();
            const auto tint = field == Field::kLevel ? Rgb(0xfff0d9) : field == Field::kHue ? hue : color;
            range->Show(Value(state, field), tint, hue);
        }
        if (rgb_) {
            for (size_t i = 0; i < kSwatches.size(); ++i)
                swatch_selection_[i].SetVisible(state.hue == kSwatches[i].hue &&
                                                state.saturation == kSwatches[i].saturation);
        } else if (light_.HasTemperature()) {
            constexpr std::array labels{"暖白", "自然", "冷白"};
            const auto temps = Temperatures(light_);
            for (size_t i = 0; i < presets_.size(); ++i) {
                auto& preset = *presets_[i];
                const bool selected = state.kelvin == temps[i];
                preset.box.SetFillColor(selected ? Rgb(0x473629) : Rgb(0x282a31));
                preset.box.SetStrokeColor(selected ? Rgb(0x8e6d49) : Rgb(0x3d3f46));
                preset.title.Set(selected ? Text::kPresetSelected : Text::kPreset, preset.origin, labels[i]);
            }
        }
    }

    bool Touch(const mp::TouchEvent& event, LightState& state, bool& changed, bool& committed) {
        for (auto& range : ranges_) {
            if (!range) continue;
            const auto update = range->Touch(event);
            if (!update.handled) continue;
            auto& value = Value(state, range->field());
            changed = value != update.value;
            value = update.value;
            committed = update.commit;
            if (changed) Show(state);
            return true;
        }
        return false;
    }

    void Cancel(LightState& state) {
        for (auto& range : ranges_)
            if (range) range->Cancel(state);
        Show(state);
    }

    bool adjusting() const {
        return std::any_of(ranges_.begin(), ranges_.end(),
                           [](const auto& range) { return range && range->dragging(); });
    }

   private:
    struct Preset {
        mp::RoundedRectNode box;
        ArtLabel title;
        Point origin;
    };

    const Art& art_;
    const Light& light_;
    bool lit_, rgb_;
    mp::RoundedRectNode ambient_, orb_;
    std::optional<ArtLabel> live_;
    std::array<std::optional<Range>, 3> ranges_;
    std::array<std::optional<Preset>, 3> presets_;
    std::array<mp::ContainerNode, 5> swatch_selection_;
};
}  // namespace light_sequence
