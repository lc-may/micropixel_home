// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

namespace micropixel::platform::audio {

using AudioStatus = esp_err_t;
inline constexpr AudioStatus kAudioOk = ESP_OK;

}  // namespace micropixel::platform::audio
