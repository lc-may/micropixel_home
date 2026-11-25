// SPDX-License-Identifier: Apache-2.0
#include "orion_audio_output.hpp"

#include <cstdio>

namespace micropixel::bouffalo {

esp_err_t OrionAudioOutput::Initialize() {
    // Bound the time the mixer can wait if the DMA channel stops making progress.
    xcodec_channel_default_config.timeout_ms = 80;
    board_ = xcodec_dev_board_init();
    if (board_ == nullptr) {
        return ESP_FAIL;
    }
    xcodec_dev_cfg_t config{
        .dev_type = XCODEC_DEV_TYPE_OUT,
        .codec_if = board_->play_codec,
        .data_if = board_->play_data,
    };
    output_ = xcodec_dev_new(&config);
    return output_ != nullptr ? ESP_OK : ESP_FAIL;
}

esp_err_t OrionAudioOutput::Start(int32_t*, uint32_t) {
    constexpr int kCodecVolume = 75;
    xcodec_dev_sample_info_t format{};
    format.sample_rate = SampleRate();
    format.bits_per_sample = 16;
    format.channel = 4;
    format.channel_mask = 0x01;
    int status = xcodec_dev_open(output_, &format);
    if (status == 0) status = xcodec_dev_set_out_vol(output_, kCodecVolume);
    if (status == 0) status = xcodec_dev_set_out_mute(output_, false);
    if (status != 0) {
        std::printf("Orion audio start failed: %d\n", status);
        return ESP_FAIL;
    }
    std::printf("Orion codec output %lu Hz, volume %d/100\n", static_cast<unsigned long>(format.sample_rate),
                kCodecVolume);
    return ESP_OK;
}

esp_err_t OrionAudioOutput::Write(const int32_t* frames, uint32_t frame_count) {
    if (frames == nullptr || frame_count > kChunkFrames || frame_count == 0U) {
        return ESP_FAIL;
    }
    bool nonzero = false;
    for (uint32_t i = 0; i < frame_count; ++i) {
        const int32_t left = frames[2U * i] >> 16;
        const int32_t right = frames[2U * i + 1U] >> 16;
        mono_[i] = static_cast<int16_t>((left + right) / 2);
        nonzero |= mono_[i] != 0;
    }
    const int status = xcodec_dev_write(output_, mono_, static_cast<int>(frame_count * sizeof(mono_[0])));
    if (status != 0) {
        // xcodec cannot report how many bytes preceded an error.
        std::printf("Orion audio DMA write failed: %d\n", status);
        return ESP_FAIL;
    }
    if (nonzero) nonzero_writes_.fetch_add(1U, std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t OrionAudioOutput::Stop() { return xcodec_dev_close(output_) == 0 ? ESP_OK : ESP_FAIL; }

void OrionAudioOutput::Shutdown() {
    if (output_ != nullptr) {
        if (Stop() != ESP_OK) {
            std::puts("Orion audio close failed");
            return;
        }
        xcodec_dev_delete(output_);
        output_ = nullptr;
    }
    if (board_ != nullptr && xcodec_dev_board_deinit() == 0) board_ = nullptr;
}

}  // namespace micropixel::bouffalo
