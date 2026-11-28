// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

#include "assets/art_data.hpp"
#include "light-sequence_assets.hpp"
#include "model.hpp"

namespace light_sequence {
using art_data::Icon;
using art_data::Surface;
using art_data::Text;
using art_data::Tone;
using mp::Color;
using mp::Container;
using mp::Point;
using mp::Rect;

inline constexpr Color Rgb(uint32_t rgb) {
    return Color::Rgb(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
}
inline constexpr auto kBackground = Rgb(0x14161a);
inline constexpr auto kEdge = Rgb(0x303238);
inline constexpr auto kGold = Rgb(0xf8cc90);

inline mp::RoundedRectNode Box(Container& canvas, Rect bounds, Color fill, uint32_t radius,
                               Color stroke = Color::Black(), uint32_t width = 0, uint8_t opacity = 255) {
    return canvas
        .CreateRoundedRect(
            bounds, {.fill = fill, .stroke = stroke, .radius = radius, .stroke_width = width, .opacity = opacity})
        .value();
}

inline void Line(Container& canvas, Rect bounds, Color color) { (void)canvas.CreateShape(bounds, color).value(); }

enum class Align { kLeft, kCenter, kRight };

// The design uses fixed font sizes down to 10 px. Rasterized glyphs retain
// those sizes on Host profiles with different system font roles.
class ArtLabel {
   public:
    static int Width(Text style, std::string_view text) {
        int width = 0;
        for (; !text.empty();) width += Next(style, text).advance64;
        return (width + 32) / 64;
    }

    void Set(Text style, Point origin, std::string_view text, Align align = Align::kLeft) {
        mp::Assert(Role(style).atlas == atlas_, "light-sequence label atlas changed");
        if (align == Align::kRight) origin.x -= Width(style, text);
        if (align == Align::kCenter) origin.x -= Width(style, text) / 2;
        int x = origin.x * 64;
        uint16_t used = 0;
        while (!text.empty()) {
            const auto& glyph = Next(style, text);
            mp::Assert(used < batch_.capacity(), "light-sequence label capacity");
            batch_.SetInstance(used++,
                               {
                                   .destination = {(x + 32) / 64 - 1, origin.y, glyph.rect.width, glyph.rect.height},
                                   .source = glyph.rect,
                               });
            x += glyph.advance64;
        }
        for (; used < batch_.capacity(); ++used) batch_.SetInstanceVisible(used, false);
    }

   private:
    ArtLabel(mp::SpriteBatch batch, int atlas) : batch_(batch), atlas_(atlas) {}

    static const art_data::RoleRange& Role(Text style) { return art_data::kRoles[static_cast<size_t>(style)]; }

    static const art_data::Glyph& Next(Text style, std::string_view& text) {
        const auto& range = Role(style);
        for (const auto& glyph : std::span{art_data::kGlyphs + range.start, static_cast<size_t>(range.length)}) {
            if (!text.starts_with(glyph.text)) continue;
            text.remove_prefix(glyph.text.size());
            return glyph;
        }
        mp::Panic(text.data());
    }

    mp::SpriteBatch batch_;
    int atlas_;
    friend class Art;
};

class Art {
   public:
    explicit Art(const mp::Application& app)
        : surfaces_{Load(app, light_sequence_assets::surfaces, "surfaces"),
                    Load(app, light_sequence_assets::lighting, "lighting")},
          icons_(Load(app, light_sequence_assets::icons, "icons")),
          text_{Load(app, light_sequence_assets::text_navigation, "text_navigation"),
                Load(app, light_sequence_assets::text_detail, "text_detail"),
                Load(app, light_sequence_assets::text_power, "text_power")} {}

    void Draw(Container& canvas, Surface art, Point at) const {
        const auto& item = art_data::kSurfaces[static_cast<size_t>(art)];
        const auto src = item.rect;
        (void)canvas.CreateSprite(surfaces_[item.atlas], {at.x, at.y, src.width, src.height}, src).value();
    }

    void Glyph(Container& canvas, Icon icon, Tone tone, int size, Point at, uint8_t opacity = 255) const {
        for (const auto& item : art_data::kIcons) {
            if (item.icon != icon || item.tone != tone || item.rect.width != size) continue;
            (void)canvas.CreateSprite(icons_, {at.x, at.y, size, size}, item.rect, opacity).value();
            return;
        }
        mp::Panic("light-sequence icon missing");
    }

    ArtLabel Label(Container& canvas, Text style, uint16_t capacity) const {
        const int atlas = ArtLabel::Role(style).atlas;
        return ArtLabel(canvas.CreateSpriteBatch(text_[atlas], capacity).value(), atlas);
    }

    void Write(Container& canvas, Text style, Point origin, std::string_view text, Align align = Align::kLeft) const {
        const auto chars =
            std::count_if(text.begin(), text.end(), [](char value) { return (uint8_t(value) & 0xc0) != 0x80; });
        Label(canvas, style, static_cast<uint16_t>(chars)).Set(style, origin, text, align);
    }

   private:
    static mp::Texture Load(const mp::Application& app, mp::AssetId id, const char* name) {
        auto texture = app.resources().LoadTexture(id, mp::TextureScale::kNative);
        if (!texture) {
            Copy message;
            message.Append("light-sequence could not load ");
            message.Append(name);
            message.Append(": ");
            message.Append(texture.error().name());
            mp::Panic(message.c_str());
        }
        return std::move(*texture);
    }

    std::array<mp::Texture, 2> surfaces_;
    mp::Texture icons_;
    std::array<mp::Texture, 3> text_;
};

inline Icon FixtureIcon(Fixture fixture) {
    constexpr std::array icons{Icon::kCeiling, Icon::kStrip, Icon::kSpot, Icon::kDine};
    return icons[static_cast<size_t>(fixture)];
}
}  // namespace light_sequence
