// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::bouffalo {

struct OrionTouchPoint final {
    int32_t x{};
    int32_t y{};
    bool pressed{};
};

class OrionTouch final {
   public:
    [[nodiscard]] bool Initialize(uint16_t width, uint16_t height);
    [[nodiscard]] OrionTouchPoint Poll();

   private:
    uint16_t width_{};
    uint16_t height_{};
    uint8_t slots_{};
    uint8_t read_failures_{};
    bool checksum_{};
    OrionTouchPoint point_{};
};

}  // namespace micropixel::bouffalo
