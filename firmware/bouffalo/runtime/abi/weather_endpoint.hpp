// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "runtime/abi/service_registry.hpp"

namespace micropixel::bouffalo {

class MediaWidgetSession;

class WeatherState final {
   public:
    void Update(const micropixel_widget_snapshot_t& value);
    bool Read(micropixel_widget_snapshot_t& value, uint32_t& revision) const;

   private:
    micropixel_widget_snapshot_t value_{};
    uint32_t revision_{};
};

class HomeWidgetEndpoint final : public runtime::ServiceHandler {
   public:
    HomeWidgetEndpoint(WeatherState* weather, MediaWidgetSession& media) : weather_(weather), media_(media) {}
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&) override;

   private:
    WeatherState* weather_;
    MediaWidgetSession& media_;
};

}  // namespace micropixel::bouffalo
