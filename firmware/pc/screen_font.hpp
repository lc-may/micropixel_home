// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "font.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
namespace micropixel::pc {
namespace graphics = platform::lvgl;
struct ScreenFont {
    std::vector<uint8_t> bytes;
    lv_font_t* font{};
    graphics::SystemFontSet guest_fonts{};
    std::array<pc::GuestFont, 4> guest_font_adapters{};
    std::vector<uint8_t> numeric_bytes;
    lv_font_t *display_time{}, *display_temperature{}, *display_alarm{};
    ScreenFont() {
        const char* configured = std::getenv("MICROPIXEL_PC_FONT");
        const std::array candidates{configured, "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                    "/usr/share/fonts/truetype/arphic-gbsn00lp/gbsn00lp.ttf"};
        for (const char* path : candidates) {
            if (!path || !*path) continue;
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size < 12 || size > 32U * 1024 * 1024) continue;
            FILE* file = std::fopen(path, "rb");
            if (!file) continue;
            bytes.resize(size);
            const bool ok = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
            std::fclose(file);
            if (ok) font = lv_tiny_ttf_create_data_ex(bytes.data(), bytes.size(), 18, LV_FONT_KERNING_NONE, 128);
            if (!font) continue;
            const std::array<int, 4> sizes{16, 20, 26, 32};
            for (size_t i = 0; i < sizes.size(); ++i) {
                guest_font_adapters[i].Initialize(
                    lv_tiny_ttf_create_data_ex(bytes.data(), bytes.size(), sizes[i], LV_FONT_KERNING_NONE, 128),
                    graphics::BuiltinLatinFont(static_cast<graphics::SystemFontRole>(i)));
                guest_fonts.fonts[i] = guest_font_adapters[i].source ? &guest_font_adapters[i].view : nullptr;
            }
            lv_obj_set_style_text_font(lv_screen_active(), font, 0);
            lv_obj_set_style_text_font(lv_layer_top(), font, 0);
            const char* numeric = "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf";
            if (FILE* file = std::fopen(numeric, "rb")) {
                if (std::fseek(file, 0, SEEK_END) == 0) {
                    auto size = std::ftell(file);
                    if (size > 0 && size < 2 * 1024 * 1024 && std::fseek(file, 0, SEEK_SET) == 0) {
                        numeric_bytes.resize(size);
                        if (std::fread(numeric_bytes.data(), 1, size, file) == static_cast<size_t>(size)) {
                            display_time =
                                lv_tiny_ttf_create_data_ex(numeric_bytes.data(), size, 96, LV_FONT_KERNING_NONE, 32);
                            display_temperature =
                                lv_tiny_ttf_create_data_ex(numeric_bytes.data(), size, 56, LV_FONT_KERNING_NONE, 32);
                            display_alarm =
                                lv_tiny_ttf_create_data_ex(numeric_bytes.data(), size, 168, LV_FONT_KERNING_NONE, 32);
                        }
                    }
                }
                std::fclose(file);
            }
            std::fprintf(stderr, "PC UI font: %s\n", path);
            return;
        }
        std::fputs("Chinese UI font unavailable: install fonts-noto-cjk or set MICROPIXEL_PC_FONT\n", stderr);
    }
    ~ScreenFont() {
        lv_obj_set_style_text_font(lv_screen_active(), &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_font(lv_layer_top(), &lv_font_montserrat_14, 0);
        for (const auto& adapter : guest_font_adapters)
            if (adapter.source) lv_tiny_ttf_destroy(adapter.source);
        if (font) lv_tiny_ttf_destroy(font);
        if (display_time) lv_tiny_ttf_destroy(display_time);
        if (display_temperature) lv_tiny_ttf_destroy(display_temperature);
        if (display_alarm) lv_tiny_ttf_destroy(display_alarm);
    }
};
}  // namespace micropixel::pc
