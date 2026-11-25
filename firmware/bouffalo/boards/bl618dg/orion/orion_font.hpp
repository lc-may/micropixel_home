// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lvgl.h"

namespace micropixel::bouffalo {
bool InitializeOrionFont();
const lv_font_t& OrionFontFallback();
}  // namespace micropixel::bouffalo
