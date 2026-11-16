// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
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
// One measured line from a potentially long text. The returned byte count is
// always a complete UTF-8 prefix; work is bounded to 512 bytes per call.
template <class Measure>
size_t FittingTextPrefix(std::string_view text, uint32_t width, Measure measure) {
    size_t end = std::min<size_t>(512, text.find('\n'));
    end = std::min(end, text.size());
    while (end < text.size() && end && (uint8_t(text[end]) & 0xc0) == 0x80) --end;
    text = text.substr(0, end);
    if (!width || !ValidUtf8(text)) return 0;
    std::array<uint16_t, 513> boundaries{};
    unsigned count = 0;
    for (size_t p = 0; p < text.size();) {
        ++p;
        while (p < text.size() && (uint8_t(text[p]) & 0xc0) == 0x80) ++p;
        boundaries[++count] = uint16_t(p);
    }
    unsigned low = 0, high = count;
    while (low < high) {
        unsigned mid = (low + high + 1) / 2;
        if (measure(text.substr(0, boundaries[mid])) <= width)
            low = mid;
        else
            high = mid - 1;
    }
    return boundaries[low];
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
        auto remaining = text.substr(at);
        size_t n = FittingTextPrefix(remaining, width, measure);
        bool clipped = line + 1 == Lines && at + n < text.size();
        if (clipped)
            n = FittingTextPrefix(remaining, width,
                                  [&](std::string_view value) { return measure(std::string(value) + "…"); });
        result[line] = text.substr(at, n);
        while (!result[line].empty() && result[line].back() == ' ') result[line].pop_back();
        if (clipped) {
            if (measure("…") <= width) result[line] += "…";
            break;
        }
        if (!n && !remaining.starts_with('\n')) break;
        at += n;
        if (at < text.size() && text[at] == '\n') ++at;
    }
    return result;
}
}  // namespace micropixel::ui
