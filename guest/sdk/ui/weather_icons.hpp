// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdk/ui/material_icons.hpp"
namespace micropixel::ui {
using WeatherAtlas = MaterialAtlas;
class WeatherIcon {
   public:
    static WeatherIcon Create(Container& parent, const WeatherAtlas& atlas, Point at, int size = 96) {
        WeatherIcon icon;
        icon.icon_ = MaterialIcon::Create(parent, atlas, at, size, MaterialSymbol::Unavailable);
        icon.Set(-1, false);
        return icon;
    }
    void Set(int code, bool known) {
        icon_.Set(static_cast<MaterialSymbol>(known ? material_art::Index(code) : 8));
        icon_.SetOpacity(known ? 255 : 100);
    }

   private:
    MaterialIcon icon_;
};
}  // namespace micropixel::ui
