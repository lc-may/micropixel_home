#pragma once

#include "platform/audio/audio_status.hpp"

namespace micropixel::platform::audio {

// Board power boundary used by the shared audio engine. The engine owns the
// idle policy; the board implementation owns rail sequencing and readiness.
class AudioPowerController {
   public:
    virtual ~AudioPowerController() = default;
    AudioPowerController(const AudioPowerController&) = delete;
    AudioPowerController& operator=(const AudioPowerController&) = delete;

    // Returns only after the board-specific audio domain is ready for its
    // codec driver to access hardware.
    [[nodiscard]] virtual AudioStatus PowerOnAudio() = 0;
    [[nodiscard]] virtual AudioStatus PowerOffAudio() = 0;

   protected:
    AudioPowerController() = default;
};

}  // namespace micropixel::platform::audio
