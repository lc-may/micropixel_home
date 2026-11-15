// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace netease {
inline std::string Clip(std::string_view text, size_t limit = 112) {
    size_t n = text.size();
    if (n > limit) {
        n = limit - 3;
        while (n && (uint8_t(text[n]) & 0xc0) == 0x80) --n;
    }
    auto result = std::string(text.substr(0, n));
    for (char& c : result)
        if (uint8_t(c) < 32) c = ' ';
    if (n < text.size()) result += "...";
    return result;
}
}  // namespace netease
