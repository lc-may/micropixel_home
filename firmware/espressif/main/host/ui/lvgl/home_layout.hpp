// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::host::home_layout {

inline constexpr int kExtent = 720;
inline constexpr uint32_t kBg = 0x101214, kSurface = 0x1b1f23, kRaised = 0x242a2f, kText = 0xf5f7f8, kMuted = 0x8c979e,
                          kAmber = 0xe8a15c;
inline constexpr uint32_t kHeroStart = 0x232a30, kHeroEnd = 0x191d21;

struct Bounds {
    int x, y, width, height;
};

inline constexpr Bounds kHero{24, 75, 672, 198};
inline constexpr Bounds kClock{24, 75, 438, 198};
inline constexpr Bounds kWeather{468, 75, 228, 198};
inline constexpr Bounds kAlarm{24, 291, 327, 174};
inline constexpr Bounds kAgenda{369, 291, 327, 174};
inline constexpr Bounds kMusic{24, 483, 327, 174};
inline constexpr Bounds kApps{369, 483, 327, 174};
inline constexpr Bounds kCatalog{24, 108, 672, 480};
inline constexpr Bounds kRefresh{612, 0, 84, 75};
inline constexpr Bounds kClockValue{54, 90, 430, 0};
inline constexpr Bounds kClockDate{54, 228, 420, 0};
inline constexpr Bounds kWeatherCity{504, 85, 150, 0};
inline constexpr Bounds kWeatherValue{504, 148, 180, 0};
inline constexpr Bounds kWeatherCondition{504, 233, 174, 0};
inline constexpr Bounds kWeatherIcon{612, 105, 54, 54};
inline constexpr Bounds kCardIcon{255, 24, 34, 34};
inline constexpr Bounds kTransport{135, 96, 168, 60};
inline constexpr Bounds kPageIndicator{344, 691, 24, 8};
inline constexpr Bounds kCatalogIndicator{377, 691, 8, 8};

constexpr Bounds AppTile(uint32_t index) {
    return {static_cast<int>(index % 2) * 348, static_cast<int>(index / 2) * 116, 324, 104};
}

}  // namespace micropixel::host::home_layout
