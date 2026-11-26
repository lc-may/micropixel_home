// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>

#include "board_audio.h"
#include "platform/audio/audio_output_peripheral.hpp"

namespace micropixel::bouffalo {

class OrionAudioOutput final : public platform::audio::AudioOutputPeripheral {
   public:
    platform::audio::AudioStatus Initialize() override;
    platform::audio::AudioStatus Start(int32_t* scratch_frames, uint32_t frame_count) override;
    platform::audio::AudioStatus Write(const int32_t* frames, uint32_t frame_count) override;
    platform::audio::AudioStatus Stop() override;
    void Shutdown() override;
    const char* Name() const override { return "Orion JY6316"; }
    uint32_t SampleRate() const override { return 16000U; }
    uint32_t NonzeroWrites() const { return nonzero_writes_.load(std::memory_order_relaxed); }

   private:
    static constexpr uint32_t kChunkFrames = 128U;
    xcodec_board_handle_t board_{};
    xcodec_dev_handle_t output_{};
    int16_t mono_[kChunkFrames]{};
    std::atomic<uint32_t> nonzero_writes_{};
};

}  // namespace micropixel::bouffalo
