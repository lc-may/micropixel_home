// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lvgl.h"
namespace micropixel::pc {
// LVGL TinyTTF returns draw buffers. The shared Guest rasterizer also asks for
// raw A8 coverage; adapt that request without changing LVGL or generated fonts.
struct GuestFont final {
    lv_font_t* source{};
    lv_font_t view{};
    void Initialize(lv_font_t* font, const lv_font_t* symbols = nullptr) {
        source = font;
        if (!source) return;
        view = *source;
        view.fallback = symbols;
        view.dsc = this;
        view.get_glyph_dsc = [](const lv_font_t* self, lv_font_glyph_dsc_t* glyph, uint32_t cp, uint32_t next) {
            auto& adapter = *static_cast<const GuestFont*>(self->dsc);
            // SDK symbols live in the built-in Latin font, not the desktop CJK font.
            if (cp >= 0xf000 && cp <= 0xf8ff) return false;
            return adapter.source->get_glyph_dsc(adapter.source, glyph, cp, next);
        };
        view.get_glyph_bitmap = [](lv_font_glyph_dsc_t* glyph, lv_draw_buf_t* target) -> const void* {
            auto& adapter = *static_cast<const GuestFont*>(glyph->resolved_font->dsc);
            auto raw = *glyph;
            raw.resolved_font = adapter.source;
            raw.req_raw_bitmap = 0;
            const auto* buffer = static_cast<const lv_draw_buf_t*>(adapter.source->get_glyph_bitmap(&raw, target));
            glyph->entry = raw.entry;
            if (!buffer) return nullptr;
            glyph->stride = buffer->header.stride;
            return glyph->req_raw_bitmap ? static_cast<const void*>(buffer->data) : buffer;
        };
        view.release_glyph = [](const lv_font_t* self, lv_font_glyph_dsc_t* glyph) {
            auto& adapter = *static_cast<const GuestFont*>(self->dsc);
            auto raw = *glyph;
            raw.resolved_font = adapter.source;
            if (adapter.source->release_glyph) adapter.source->release_glyph(adapter.source, &raw);
            glyph->entry = nullptr;
        };
    }
};
}  // namespace micropixel::pc
