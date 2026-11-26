// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "abi/micropixel_abi.h"
namespace micropixel::host {
class SystemClock final {
   public:
    struct Settings {
        int32_t offset_minutes{480};
        bool enabled{true};
        int64_t correction{};
    };
    bool Restore(Settings settings) {
        if (settings.offset_minutes < -720 || settings.offset_minutes > 840 || settings.correction < -4102444800LL ||
            settings.correction > 4102444800LL)
            return false;
        settings_ = settings;
        return true;
    }
    const Settings& Configuration() const { return settings_; }
    micropixel_wall_time_t Now(uint64_t system_utc) const {
        const int64_t corrected = static_cast<int64_t>(system_utc) + settings_.correction;
        const bool valid = settings_.enabled && system_utc >= 1577836800ULL && system_utc <= 4102444800ULL &&
                           corrected >= 1577836800LL && corrected <= 4102444800LL;
        return {sizeof(micropixel_wall_time_t),
                static_cast<uint16_t>(valid),
                0,
                valid ? static_cast<uint64_t>(corrected) : 0,
                settings_.offset_minutes,
                0};
    }

   private:
    Settings settings_{};
};
}  // namespace micropixel::host
