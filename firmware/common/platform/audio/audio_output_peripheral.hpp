#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/audio/audio_status.hpp"

namespace micropixel::platform::audio {

// Hardware output boundary for the shared mixer. Samples are signed stereo
// 32-bit frames with the 16-bit PCM value in the high half-word.
class AudioOutputPeripheral {
   public:
    virtual ~AudioOutputPeripheral() = default;
    AudioOutputPeripheral(const AudioOutputPeripheral&) = delete;
    AudioOutputPeripheral& operator=(const AudioOutputPeripheral&) = delete;

    [[nodiscard]] virtual AudioStatus Initialize() = 0;
    [[nodiscard]] virtual AudioStatus Start(int32_t* scratch_frames, uint32_t frame_count) = 0;
    [[nodiscard]] virtual AudioStatus Write(const int32_t* frames, uint32_t frame_count) = 0;
    [[nodiscard]] virtual AudioStatus Stop() = 0;
    virtual void Shutdown() = 0;
    [[nodiscard]] virtual const char* Name() const = 0;
    // Frame rate the peripheral clocks out; the mixer renders at exactly this
    // rate, so it is the board's single source of truth for the mix rate.
    [[nodiscard]] virtual uint32_t SampleRate() const = 0;

   protected:
    AudioOutputPeripheral() = default;
};

}  // namespace micropixel::platform::audio
