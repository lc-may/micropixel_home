// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel {
namespace device {
class UrlMedia;
}
namespace platform::audio {
class AudioEngine;
}

namespace bouffalo {
bool InitializeMedia(platform::audio::AudioEngine& audio, uint32_t sample_rate);
device::UrlMedia* MediaBackend();
}  // namespace bouffalo
}  // namespace micropixel
