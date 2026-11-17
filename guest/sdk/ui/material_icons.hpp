// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <memory>

#include "sdk/application.hpp"
#include "sdk/panic.hpp"
#include "sdk/scene.hpp"
#include "sdk/ui/button.hpp"
#include "sdk/ui/material_icons_data.hpp"
namespace micropixel::ui {
using MaterialSymbol = material_art::Symbol;
class MaterialAtlas {
   public:
    explicit MaterialAtlas(const Application& app) {
        using namespace material_art;
        auto pixels = std::make_unique<uint8_t[]>(Width * Height * 4);
        Assert(Expand({pixels.get(), Width * Height * 4}), "material atlas invalid");
        texture_ = app.resources()
                       .CreateDynamicTexture({Width, Height}, PixelFormat::kBgra8888,
                                             {pixels.get(), Width * Height * 4}, Width * 4)
                       .value();
    }
    const Texture& texture() const { return texture_; }

   private:
    Texture texture_;
};
class MaterialIcon {
   public:
    static MaterialIcon Create(Container& parent, const MaterialAtlas& atlas, Point at, int size,
                               MaterialSymbol symbol) {
        using namespace material_art;
        MaterialIcon icon;
        icon.root_ = parent.CreateContainer({.translation = at}).value();
        icon.sprite_ = icon.root_.CreateSprite(atlas.texture(), {0, 0, size, size}, {0, 0, Cell, Cell}).value();
        icon.Set(symbol);
        return icon;
    }
    void Set(MaterialSymbol symbol) {
        using namespace material_art;
        unsigned i = static_cast<unsigned>(symbol);
        if (i == index_) return;
        index_ = i;
        sprite_.SetSource({int(i % Columns) * int(Cell), int(i / Columns) * int(Cell), Cell, Cell});
    }
    void SetOpacity(uint8_t alpha) { root_.SetOpacity(alpha); }
    void SetPosition(Point position) { root_.SetTranslation(position); }

   private:
    ContainerNode root_;
    SpriteNode sprite_;
    unsigned index_{99};
};
class MaterialIconButton {
   public:
    static MaterialIconButton Create(Container& parent, const MaterialAtlas& atlas, Rect bounds, MaterialSymbol symbol,
                                     Color background = Color::Rgb(27, 31, 35)) {
        MaterialIconButton button;
        button.root_ = parent.CreateContainer({.translation = {bounds.x, bounds.y}}).value();
        button.hit_ = Button({0, 0, bounds.width, bounds.height});
        button.hit_.SetHitPadding(std::max(0, (85 - std::min(bounds.width, bounds.height)) / 2));
        button.background_ =
            button.root_.CreateRoundedRect({0, 0, bounds.width, bounds.height}, {.fill = background, .radius = 24})
                .value();
        button.icon_ =
            MaterialIcon::Create(button.root_, atlas, {(bounds.width - 34) / 2, (bounds.height - 34) / 2}, 34, symbol);
        return button;
    }
    bool valid() const { return root_.valid(); }
    ButtonUpdate OnTouch(const TouchEvent& event) {
        if (!valid() || !visible_ || pending_) return {};
        auto value = hit_.OnTouch(event.WithPosition(root_.ToLocal(event.position())));
        return value;
    }
    void SetVisible(bool visible) {
        if (!valid()) return;
        visible_ = visible;
        root_.SetVisible(visible);
        if (!visible) hit_.Reset();
    }
    void SetEnabled(bool enabled) {
        enabled_ = enabled;
        SyncEnabled();
    }
    void SetPending(bool pending) {
        pending_ = pending;
        SyncEnabled();
    }
    void SetIcon(MaterialSymbol symbol) {
        if (valid()) icon_.Set(symbol);
    }
    Result<void> SetBounds(Rect bounds) {
        if (bounds.empty()) return unexpected(Error{ErrorCode::kInvalidArgument});
        if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
        root_.SetTranslation({bounds.x, bounds.y});
        background_.SetRect({0, 0, bounds.width, bounds.height});
        icon_.SetPosition({(bounds.width - 34) / 2, (bounds.height - 34) / 2});
        hit_.SetBounds({0, 0, bounds.width, bounds.height});
        hit_.SetHitPadding(std::max(0, (85 - std::min(bounds.width, bounds.height)) / 2));
        return {};
    }
    void Reset() { hit_.Reset(); }

   private:
    void SyncEnabled() {
        hit_.SetEnabled(enabled_ && !pending_);
        if (valid()) root_.SetOpacity(enabled_ && !pending_ ? 255 : 100);
    }
    ContainerNode root_;
    RoundedRectNode background_;
    Button hit_;
    MaterialIcon icon_;
    bool visible_{true}, enabled_{true}, pending_{};
};
}  // namespace micropixel::ui
