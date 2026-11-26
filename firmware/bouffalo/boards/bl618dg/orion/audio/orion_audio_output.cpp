// SPDX-License-Identifier: Apache-2.0
#include "orion_audio_output.hpp"

#include <cstdio>

#include "xcodec_dev_types.h"

namespace micropixel::bouffalo {

platform::audio::AudioStatus OrionAudioOutput::Initialize() {
    // Bound the time the mixer can wait if the DMA channel stops making progress.
    xcodec_channel_default_config.timeout_ms = 80;
    board_ = xcodec_dev_board_init();
    if (board_ == nullptr) {
        return XCODEC_DEV_NOT_FOUND;
    }
    xcodec_dev_cfg_t config{
        .dev_type = XCODEC_DEV_TYPE_OUT,
        .codec_if = board_->play_codec,
        .data_if = board_->play_data,
    };
    output_ = xcodec_dev_new(&config);
    return output_ != nullptr ? XCODEC_DEV_OK : XCODEC_DEV_NO_MEM;
}

platform::audio::AudioStatus OrionAudioOutput::Start(int32_t*, uint32_t) {
    constexpr int kCodecVolume = 75;
    xcodec_dev_sample_info_t format{};
    format.sample_rate = SampleRate();
    format.bits_per_sample = 16;
    format.channel = 4;
    format.channel_mask = 0x01;
    int status = xcodec_dev_open(output_, &format);
    if (status == XCODEC_DEV_OK) status = xcodec_dev_set_out_vol(output_, kCodecVolume);
    if (status == XCODEC_DEV_OK) status = xcodec_dev_set_out_mute(output_, false);
    if (status != XCODEC_DEV_OK) {
        std::printf("Orion audio start failed: %d\n", status);
        return status;
    }
    std::printf("Orion codec output %lu Hz, volume %d/100\n", static_cast<unsigned long>(format.sample_rate),
                kCodecVolume);
    return XCODEC_DEV_OK;
}

platform::audio::AudioStatus OrionAudioOutput::Write(const int32_t* frames, uint32_t frame_count) {
    if (frames == nullptr || frame_count > kChunkFrames || frame_count == 0U) {
        return XCODEC_DEV_INVALID_ARG;
    }
    bool nonzero = false;
    for (uint32_t i = 0; i < frame_count; ++i) {
        const int32_t left = frames[2U * i] >> 16;
        const int32_t right = frames[2U * i + 1U] >> 16;
        mono_[i] = static_cast<int16_t>((left + right) / 2);
        nonzero |= mono_[i] != 0;
    }
    const int status = xcodec_dev_write(output_, mono_, static_cast<int>(frame_count * sizeof(mono_[0])));
    if (status != XCODEC_DEV_OK) {
        // xcodec cannot report how many bytes preceded an error.
        std::printf("Orion audio DMA write failed: %d\n", status);
        return status;
    }
    if (nonzero) nonzero_writes_.fetch_add(1U, std::memory_order_relaxed);
    return XCODEC_DEV_OK;
}

platform::audio::AudioStatus OrionAudioOutput::Stop() { return xcodec_dev_close(output_); }

void OrionAudioOutput::Shutdown() {
    if (output_ != nullptr) {
        if (Stop() != XCODEC_DEV_OK) {
            std::puts("Orion audio close failed");
            return;
        }
        xcodec_dev_delete(output_);
        output_ = nullptr;
    }
    if (board_ != nullptr && xcodec_dev_board_deinit() == XCODEC_DEV_OK) board_ = nullptr;
}

}  // namespace micropixel::bouffalo
