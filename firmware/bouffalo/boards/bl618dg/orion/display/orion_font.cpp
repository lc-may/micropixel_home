// SPDX-License-Identifier: Apache-2.0
#include "orion_font.hpp"

#include <cstdio>
#include <cstring>

#include "mm.h"
#include "orion_font_data.h"
#include "platform/storage/embedded_xz.hpp"

LV_FONT_DECLARE(font_orion_cjk)

namespace micropixel::bouffalo {
namespace {

// Matches the little-endian record in build_orion_font.py. Packed bitmaps
// begin after the sorted index and have no row padding.
struct Glyph final {
    uint32_t codepoint;
    uint32_t offset;
    uint8_t advance;
    uint8_t width;
    uint8_t height;
    int8_t x;
    int8_t y;
    uint8_t reserved[3];
};
static_assert(sizeof(Glyph) == 16);
constexpr uint32_t kIndexBytes = kCoverageCount * sizeof(Glyph);
static_assert(kIndexBytes < kCoverageBytes);

const uint8_t* coverage;
lv_font_t expanded;
lv_font_t combined;

Glyph ReadGlyph(uint32_t index) {
    Glyph glyph;
    std::memcpy(&glyph, coverage + index * sizeof(glyph), sizeof(glyph));
    return glyph;
}

bool DescribeGlyph(const lv_font_t*, lv_font_glyph_dsc_t* descriptor, uint32_t codepoint, uint32_t) {
    uint32_t first = 0;
    uint32_t last = kCoverageCount;
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        const auto glyph = ReadGlyph(middle);
        if (glyph.codepoint < codepoint) {
            first = middle + 1;
        } else if (glyph.codepoint > codepoint) {
            last = middle;
        } else {
            descriptor->adv_w = glyph.advance;
            descriptor->box_w = glyph.width;
            descriptor->box_h = glyph.height;
            descriptor->ofs_x = glyph.x;
            descriptor->ofs_y = glyph.y;
            descriptor->stride = 0;
            descriptor->format = LV_FONT_GLYPH_FORMAT_A1;
            descriptor->is_placeholder = 0;
            descriptor->gid.index = middle;
            return true;
        }
    }
    return false;
}

const void* GlyphBitmap(lv_font_glyph_dsc_t* descriptor, lv_draw_buf_t* draw_buffer) {
    const auto glyph = ReadGlyph(descriptor->gid.index);
    const auto* bitmap = coverage + kIndexBytes + glyph.offset;
    if (descriptor->req_raw_bitmap) return bitmap;
    if (!draw_buffer || !glyph.width || !glyph.height) return nullptr;
    const auto stride = lv_draw_buf_width_to_stride(glyph.width, LV_COLOR_FORMAT_A8);
    for (uint32_t row = 0; row < glyph.height; ++row) {
        auto* pixels = draw_buffer->data + row * stride;
        for (uint32_t column = 0; column < glyph.width; ++column) {
            const auto bit = row * glyph.width + column;
            pixels[column] = (bitmap[bit / 8U] & (0x80U >> (bit % 8U))) ? UINT8_MAX : 0;
        }
    }
    lv_draw_buf_flush_cache(draw_buffer, nullptr);
    return draw_buffer;
}

}  // namespace

bool InitializeOrionFont() {
    auto* data = static_cast<uint8_t*>(kmalloc(kCoverageBytes, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    if (!data) return false;
    if (!DecodeEmbeddedXz(kCompressedCoverage, {data, kCoverageBytes})) {
        kfree(data);
        return false;
    }
    coverage = data;
    expanded = {.get_glyph_dsc = DescribeGlyph,
                .get_glyph_bitmap = GlyphBitmap,
                .line_height = font_orion_cjk.line_height,
                .base_line = font_orion_cjk.base_line};
    combined = font_orion_cjk;
    combined.fallback = &expanded;
    std::printf("Orion font ready: %u codepoints (%u Han), %u PSRAM bytes\n", kCoverageCount, kHanCount,
                kCoverageBytes);
    return true;
}

const lv_font_t& OrionFontFallback() { return coverage ? combined : font_orion_cjk; }

}  // namespace micropixel::bouffalo
