// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <span>

#include "sdk/math.hpp"
#include "sdk/micropixel.hpp"

namespace light_sequence {
namespace mp = micropixel;
namespace ui = mp::ui;

enum class Fixture { kCeiling, kStrip, kSpot, kDine };
enum class Kind { kSwitch, kDim, kWhite, kRgbw };

struct LightState {
    bool on{};
    int level{100};
    int kelvin{};
    int hue{28};
    int saturation{76};
    bool rgb{};
};

struct Light {
    const char* name;
    Fixture fixture;
    Kind kind;
    LightState initial;
    bool online{true};
    int minimum{}, maximum{};

    bool HasLevel() const { return kind != Kind::kSwitch; }
    bool HasTemperature() const { return kind == Kind::kWhite || kind == Kind::kRgbw; }
    bool HasColor() const { return kind == Kind::kRgbw; }
    const char* type() const {
        constexpr std::array names{"开关灯", "亮度可调", "冷暖调光", "彩色 + 冷暖调光"};
        return names[static_cast<size_t>(kind)];
    }
};

inline constexpr std::array kLights{
    Light{"客厅主灯", Fixture::kCeiling, Kind::kWhite, {true, 68, 3800}, true, 2700, 6500},
    Light{"氛围灯带", Fixture::kStrip, Kind::kRgbw, {true, 46, 3000, 28, 76, true}, true, 2700, 6500},
    Light{"左射灯", Fixture::kSpot, Kind::kDim, {false, 60}},
    Light{"右射灯", Fixture::kSpot, Kind::kDim, {false, 60}},
    Light{"主卧吸顶灯", Fixture::kCeiling, Kind::kWhite, {false, 54, 3200}, true, 2200, 5000},
    Light{"阅读壁灯", Fixture::kSpot, Kind::kDim, {false, 40}},
    Light{"床头灯带", Fixture::kStrip, Kind::kWhite, {false, 32, 2700}, true, 2700, 5500},
    Light{"餐桌吊灯", Fixture::kDine, Kind::kWhite, {true, 72, 3000}, true, 2700, 5000},
    Light{"餐边柜灯", Fixture::kStrip, Kind::kDim, {false, 42}, false},
    Light{"玄关顶灯", Fixture::kCeiling, Kind::kSwitch, {true}},
    Light{"鞋柜灯", Fixture::kStrip, Kind::kSwitch, {true}},
};

struct Group {
    const char* name;
    std::array<int, 2> members;
};

inline constexpr std::array kGroups{
    Group{"射灯组", {2, 3}},
    Group{"氛围灯组", {1, 3}},
    Group{"床头灯组", {5, 6}},
};

struct Room {
    const char* name;
    std::span<const int> lights;
    std::span<const int> groups;
};

inline constexpr int kLiving[]{0, 1, 2, 3}, kBedroom[]{4, 5, 6}, kDining[]{7, 8}, kEntry[]{9, 10};
inline constexpr int kLivingGroups[]{0, 1}, kBedroomGroups[]{2};
inline constexpr std::array kRooms{
    Room{"客厅", kLiving, kLivingGroups},
    Room{"主卧", kBedroom, kBedroomGroups},
    Room{"餐厅", kDining, {}},
    Room{"玄关", kEntry, {}},
};

struct Counts {
    int total{}, on{}, offline{};
    int online() const { return total - offline; }
};

struct Swatch {
    int hue, saturation;
};

inline constexpr std::array kSwatches{Swatch{28, 76}, Swatch{4, 73}, Swatch{140, 61}, Swatch{200, 65}, Swatch{268, 61}};

inline std::array<int, 3> Temperatures(const Light& light) {
    return {light.minimum, std::clamp(4000, light.minimum + 100, light.maximum - 100), light.maximum};
}

class Model {
   public:
    LightState& operator[](size_t index) { return states_[index]; }
    const LightState& operator[](size_t index) const { return states_[index]; }

    void Load(const mp::Application& app) {
        std::array<uint8_t, kLights.size() * kRecordBytes> bytes{};
        auto read = app.storage().GetBytes(kStorageKey, bytes.data(), bytes.size());
        const bool saved = read && *read == bytes.size();
        for (size_t i = 0; i < kLights.size(); ++i) {
            const auto& light = kLights[i];
            auto& state = states_[i];
            state = light.initial;
            if (!saved) continue;
            const auto* record = bytes.data() + i * kRecordBytes;
            state.on = light.online && (record[0] & 1U);
            state.rgb = light.HasColor() && (record[0] & 2U);
            state.level = std::clamp<int>(record[1], 1, 100);
            state.kelvin = std::clamp(100 * ((Read16(record + 2) + 50) / 100), light.minimum, light.maximum);
            state.hue = std::clamp(Read16(record + 4), 0, 359);
            state.saturation = std::clamp<int>(record[6], 0, 100);
        }
    }

    mp::Result<void> Save(const mp::Application& app) const {
        std::array<uint8_t, kLights.size() * kRecordBytes> bytes{};
        for (size_t i = 0; i < states_.size(); ++i) {
            const auto& state = states_[i];
            auto* record = bytes.data() + i * kRecordBytes;
            record[0] = (state.on ? 1U : 0U) | (state.rgb ? 2U : 0U);
            record[1] = static_cast<uint8_t>(state.level);
            Write16(record + 2, state.kelvin);
            Write16(record + 4, state.hue);
            record[6] = static_cast<uint8_t>(state.saturation);
        }
        return app.storage().SetBytes(kStorageKey, bytes.data(), bytes.size());
    }

    Counts Count(std::span<const int> lights) const {
        Counts result{.total = static_cast<int>(lights.size())};
        for (int id : lights) {
            if (!kLights[id].online)
                ++result.offline;
            else if (states_[id].on)
                ++result.on;
        }
        return result;
    }

    int TotalLit() const {
        return static_cast<int>(
            std::count_if(states_.begin(), states_.end(), [](const auto& state) { return state.on; }));
    }

    int SetBatch(std::span<const int> lights, bool on) {
        int changed = 0;
        for (int id : lights) {
            if (!kLights[id].online || states_[id].on == on) continue;
            states_[id].on = on;
            ++changed;
        }
        return changed;
    }

   private:
    static constexpr char kStorageKey[] = "lights-v1";
    static constexpr size_t kRecordBytes = 7;

    static int Read16(const uint8_t* bytes) { return bytes[0] | (static_cast<int>(bytes[1]) << 8); }
    static void Write16(uint8_t* bytes, int value) {
        bytes[0] = static_cast<uint8_t>(value);
        bytes[1] = static_cast<uint8_t>(value >> 8);
    }

    std::array<LightState, kLights.size()> states_{};
};

using Copy = mp::FixedString<96>;

inline Copy CountNote(Counts counts) {
    Copy text;
    text.AppendInt(counts.on);
    text.Append("/");
    text.AppendInt(counts.total);
    text.Append(" 已亮");
    if (counts.offline) {
        text.Append(" · ");
        text.AppendInt(counts.offline);
        text.Append(" 离线");
    }
    return text;
}

inline Copy LightNote(const Light& light, const LightState& state) {
    Copy text;
    if (!light.online) {
        text.Append("离线 · 无法控制");
    } else if (!state.on) {
        text.Append("已关闭 · ");
        text.Append(light.type());
    } else if (!light.HasLevel()) {
        text.Append("已开启");
    } else {
        text.AppendInt(state.level);
        if (state.rgb)
            text.Append("% · 彩光");
        else if (light.HasTemperature()) {
            text.Append("% · ");
            text.AppendInt(state.kelvin);
            text.Append(" K");
        } else
            text.Append("% · 亮度");
    }
    return text;
}

inline mp::Color Mix(mp::Color from, mp::Color to, float amount) {
    const auto channel = [amount](uint8_t a, uint8_t b) {
        return static_cast<uint8_t>(mp::math::RoundToInt(mp::math::Lerp(a, b, amount)));
    };
    return mp::Color::Rgb(channel(from.red(), to.red()), channel(from.green(), to.green()),
                          channel(from.blue(), to.blue()));
}

inline mp::Color HueColor(int hue, int saturation) {
    const float chroma = saturation / 100.0F;
    const float t = (hue % 60) / 60.0F;
    const int sector = hue / 60;
    const float edge = chroma * (sector % 2 ? 1 - t : t);
    const std::array<std::array<float, 3>, 6> channels{{{chroma, edge, 0},
                                                        {edge, chroma, 0},
                                                        {0, chroma, edge},
                                                        {0, edge, chroma},
                                                        {edge, 0, chroma},
                                                        {chroma, 0, edge}}};
    const auto channel = [chroma](float value) {
        return static_cast<uint8_t>(mp::math::RoundToInt((value + 1 - chroma) * 255));
    };
    const auto& rgb = channels[sector];
    return mp::Color::Rgb(channel(rgb[0]), channel(rgb[1]), channel(rgb[2]));
}

inline mp::Color WhiteColor(int kelvin) {
    constexpr auto warm = mp::Color::Rgb(255, 163, 79);
    constexpr auto neutral = mp::Color::Rgb(255, 232, 195);
    constexpr auto cool = mp::Color::Rgb(190, 219, 255);
    if (kelvin <= 4000) return Mix(warm, neutral, std::clamp((kelvin - 2200) / 1800.0F, 0.0F, 1.0F));
    return Mix(neutral, cool, std::clamp((kelvin - 4000) / 2500.0F, 0.0F, 1.0F));
}

inline mp::Color LightColor(const Light& light, const LightState& state) {
    if (state.rgb) return HueColor(state.hue, state.saturation);
    if (light.HasTemperature()) return WhiteColor(state.kelvin);
    return mp::Color::Rgb(255, 212, 160);
}

inline Copy PreviewValue(const Light& light, const LightState& state, mp::Color color) {
    Copy text;
    if (!light.online)
        text.Append("设备离线");
    else if (!state.on)
        text.Append("灯光已关闭");
    else if (state.rgb) {
        constexpr char digits[] = "0123456789ABCDEF";
        char hex[]{'#', 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 6; ++i) hex[i + 1] = digits[(color.rgb888() >> (20 - i * 4)) & 15U];
        text.Append(hex);
    } else if (light.HasTemperature()) {
        text.AppendInt(state.kelvin / 1000);
        text.Append(",");
        text.AppendInt(state.kelvin / 100 % 10);
        text.AppendInt(state.kelvin / 10 % 10);
        text.AppendInt(state.kelvin % 10);
        text.Append(" K");
    } else if (light.HasLevel()) {
        text.AppendInt(state.level);
        text.Append("%");
    } else
        text.Append("已开启");
    return text;
}
}  // namespace light_sequence
