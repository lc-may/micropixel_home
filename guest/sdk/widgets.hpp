// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string_view>

#include "sdk/result.hpp"
namespace micropixel {
enum class WidgetTemplate : uint16_t { kWeather = 1, kClock = 2, kMedia = 3 };
struct WidgetData {
    WidgetTemplate kind{WidgetTemplate::kMedia};
    std::string_view title{}, subtitle{};
    uint64_t updated_utc{};
    uint32_t valid_for_seconds{900}, position_ms{}, duration_ms{};
    int32_t value{}, auxiliary{};
    bool playing{}, failed{};
};
class Widgets final {
   public:
    [[nodiscard]] Result<uint32_t> Publish(uint32_t id, const WidgetData&) const;
};
}  // namespace micropixel
