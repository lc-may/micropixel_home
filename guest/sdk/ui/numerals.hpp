// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <memory>

#include "sdk/micropixel.hpp"
#include "sdk/ui/numerals_data.hpp"

namespace micropixel::ui {
// One activity-owned texture; all numbers in its Scene share the same glyphs.
class NumeralAtlas {
   public:
    explicit NumeralAtlas(const Application& app, Color color = Color::Rgb(245, 247, 248), int raster_height = 128) {
        using namespace numerals_data;
        constexpr size_t count = Width * Height;
        auto pixels = std::make_unique<uint8_t[]>(count * 4);
        size_t at = 0;
        for (size_t i = 0; i < Coverage.size(); i += 2) {
            Assert(at + Coverage[i] <= count, "numeric atlas coverage invalid");
            for (unsigned j = 0; j < Coverage[i]; ++j, ++at) {
                pixels[at * 4] = color.blue();
                pixels[at * 4 + 1] = color.green();
                pixels[at * 4 + 2] = color.red();
                pixels[at * 4 + 3] = Coverage[i + 1];
            }
        }
        Assert(at == count, "numeric atlas incomplete");
        Assert(raster_height == 96 || raster_height == 128, "numeric raster height invalid");
        if (raster_height == 96) {
            cell_width_ = 60;
            cell_height_ = 96;
            constexpr int width = 240, height = 384;
            auto resized = std::make_unique<uint8_t[]>(width * height * 4);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    // Bilinear coverage avoids jagged edges in the 96 px clock role.
                    float sx = (x + .5f) * 4 / 3 - .5f, sy = (y + .5f) * 4 / 3 - .5f;
                    int x0 = std::clamp(int(sx), 0, Width - 1), y0 = std::clamp(int(sy), 0, Height - 1);
                    int x1 = std::min(x0 + 1, Width - 1), y1 = std::min(y0 + 1, Height - 1);
                    float fx = std::max(0.f, sx - x0), fy = std::max(0.f, sy - y0);
                    auto alpha = [&](int px, int py) { return pixels[(py * Width + px) * 4 + 3]; };
                    auto offset = (y * width + x) * 4;
                    resized[offset] = color.blue();
                    resized[offset + 1] = color.green();
                    resized[offset + 2] = color.red();
                    resized[offset + 3] = uint8_t((alpha(x0, y0) * (1 - fx) + alpha(x1, y0) * fx) * (1 - fy) +
                                                  (alpha(x0, y1) * (1 - fx) + alpha(x1, y1) * fx) * fy + .5f);
                }
            texture_ = app.resources()
                           .CreateDynamicTexture({width, height}, PixelFormat::kBgra8888,
                                                 {resized.get(), width * height * 4}, width * 4)
                           .value();
            return;
        }
        texture_ =
            app.resources()
                .CreateDynamicTexture({Width, Height}, PixelFormat::kBgra8888, {pixels.get(), count * 4}, Width * 4)
                .value();
    }
    const Texture& texture() const { return texture_; }
    int cell_width() const { return cell_width_; }
    int cell_height() const { return cell_height_; }

   private:
    Texture texture_;
    int cell_width_{80}, cell_height_{128};
};
class Numerals {
   public:
    static Numerals Create(Container& parent, const NumeralAtlas& font, Point point, int height,
                           unsigned capacity = 6) {
        Assert(height >= 18 && height <= 168 && capacity && capacity <= 8, "numeric label bounds invalid");
        Numerals result;
        result.root_ = parent.CreateContainer({.translation = point}).value();
        result.height_ = height;
        result.capacity_ = capacity;
        result.cell_width_ = font.cell_width();
        result.cell_height_ = font.cell_height();
        for (unsigned i = 0; i < capacity; ++i) {
            result.glyphs_[i] =
                result.root_.CreateSprite(font.texture(), {0, 0, 1, 1}, {0, 0, result.cell_width_, result.cell_height_})
                    .value();
            result.glyphs_[i].SetVisible(false);
        }
        return result;
    }
    void Set(const char* text) {
        if (std::string_view(text) == last_.c_str()) return;
        last_.Clear();
        last_.Append(text);
        int x = 0;
        for (unsigned i = 0; i < capacity_; ++i) {
            if (!*text) {
                glyphs_[i].SetVisible(false);
                continue;
            }
            char ch = *text++;
            unsigned index = ch >= '0' && ch <= '9' ? unsigned(ch - '0')
                             : ch == ':'            ? 10
                             : ch == '.'            ? 11
                             : ch == '-'            ? 12
                                                    : 13;
            if (uint8_t(ch) == 0xc2 && uint8_t(*text) == 0xb0) ++text;
            glyphs_[i].SetSource(
                {int(index % 4) * cell_width_, int(index / 4) * cell_height_, cell_width_, cell_height_});
            glyphs_[i].SetDestination({x, 0, height_ * 80 / 128, height_});
            glyphs_[i].SetVisible(true);
            x += (numerals_data::Advances[index] * height_ + 64) / 128;
        }
    }
    void SetOpacity(uint8_t value) { root_.SetOpacity(value); }

   private:
    ContainerNode root_;
    std::array<SpriteNode, 8> glyphs_;
    FixedString<33> last_;
    int height_{};
    int cell_width_{}, cell_height_{};
    unsigned capacity_{};
};
}  // namespace micropixel::ui
