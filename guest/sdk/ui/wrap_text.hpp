// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace micropixel::ui {
inline bool ValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        uint32_t c = uint8_t(text[i++]);
        if (c < 128) continue;
        unsigned extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!extra || i + extra > text.size()) return false;
        uint32_t cp = c & ((1U << (6 - extra)) - 1);
        for (unsigned j = 0; j < extra; ++j) {
            auto next = uint8_t(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 63);
        }
        if (cp < (extra == 1 ? 128U : extra == 2 ? 2048U : 65536U) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
// Bounded Unicode wrapping with measured pixel widths and last-line ellipsis.
// Measure accepts string_view and returns a pixel width; no Host font dependency.
template <size_t Lines, class Measure>
std::array<std::string, Lines> WrapText(std::string_view text, uint32_t width, Measure measure) {
    std::array<std::string, Lines> result;
    if (!width || text.size() > 512 || !ValidUtf8(text)) return result;
    size_t at = 0;
    for (size_t line = 0; line < Lines && at < text.size(); ++line) {
        while (at < text.size() && text[at] == ' ') ++at;
        auto end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::array<uint16_t, 513> boundaries{};
        unsigned count = 0;
        for (size_t p = at; p < end;) {
            ++p;
            while (p < end && (uint8_t(text[p]) & 0xc0) == 0x80) ++p;
            boundaries[++count] = uint16_t(p - at);
        }
        auto fit = [&](bool ellipsis) {
            unsigned low = 0, high = count;
            while (low < high) {
                unsigned mid = (low + high + 1) / 2;
                std::string candidate(text.substr(at, boundaries[mid]));
                if (ellipsis) candidate += "…";
                if (measure(candidate) <= width)
                    low = mid;
                else
                    high = mid - 1;
            }
            return low;
        };
        unsigned n = fit(false);
        bool clipped = line + 1 == Lines && at + boundaries[n] < text.size();
        if (clipped) n = fit(true);
        result[line] = text.substr(at, boundaries[n]);
        while (!result[line].empty() && result[line].back() == ' ') result[line].pop_back();
        if (clipped) {
            if (measure("…") <= width) result[line] += "…";
            break;
        }
        if (!n && count) break;
        at += boundaries[n];
        if (at == end && at < text.size()) ++at;
    }
    return result;
}
}  // namespace micropixel::ui
