// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>

namespace micropixel::bouffalo {
bool DecodeEmbeddedXz(std::span<const uint8_t> input, std::span<uint8_t> output);
}
