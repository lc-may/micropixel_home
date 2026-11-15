// SPDX-License-Identifier: Apache-2.0
// Protocol adapted from api-enhanced (MIT); see THIRD_PARTY_NOTICES.md.
#pragma once
#include <array>
#include <string>
#include <string_view>

#include "sdk/random.hpp"
namespace netease {
std::string AesBase64(std::string_view, const std::array<uint8_t, 16>&);
std::string RsaSecret(const std::array<uint8_t, 16>&);
std::string Weapi(std::string_view, micropixel::Random);
}  // namespace netease
