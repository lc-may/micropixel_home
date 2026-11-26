#ifndef MICROPIXEL_PLATFORM_AUDIO_AUDIO_ENGINE_HPP
#define MICROPIXEL_PLATFORM_AUDIO_AUDIO_ENGINE_HPP

#include <cstdint>

#include "device/contracts/audio.hpp"
#include "platform/audio/audio_status.hpp"

#if !defined(MICROPIXEL_BOUFFALO)
#include "esp_attr.h"
#endif

namespace micropixel::platform::audio {

class AudioOutputPeripheral;
class AudioPowerController;

// Fixed-capacity Host audio engine shared by boards. A firmware image owns one
// instance and injects only its board-specific AudioOutputPeripheral.
class AudioEngine : public device::Audio {
   public:
    [[nodiscard]] AudioStatus Initialize(AudioOutputPeripheral& output, uint32_t sample_rate,
                                         AudioPowerController* power_controller = nullptr);
    virtual void SetMasterVolumePercent(uint8_t percent);
    // Board-owned switches may mute the Host mix independently of the saved
    // master-volume setting. ISR-safe: only stores a flag in internal RAM;
    // the audio task applies it to samples. Outside the Guest contract.
#if defined(MICROPIXEL_BOUFFALO)
    void SetHardwareMuted(bool muted);
#else
    void IRAM_ATTR SetHardwareMuted(bool muted);
#endif

    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override;
    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t& tone) override;
    [[nodiscard]] int32_t PlayToneForOwner(const micropixel_audio_tone_t& tone, uint32_t owner);
    [[nodiscard]] int32_t StopTonesForOwner(uint32_t owner);
    [[nodiscard]] int32_t StartPcm(const device::PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                                   device::PcmStreamHandle& handle_out) override;
    [[nodiscard]] int32_t PausePcm(device::PcmStreamHandle handle) override;
    [[nodiscard]] int32_t ResumePcm(device::PcmStreamHandle handle) override;
    [[nodiscard]] int32_t SetPcmVolume(device::PcmStreamHandle handle, uint16_t volume_per_mille) override;
    [[nodiscard]] int32_t StopPcm(device::PcmStreamHandle handle) override;
    void BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) override;
    void UnbindPcmCompletionSink(void* context) override;
    [[nodiscard]] int32_t StopAll() override;
    // GuestContext uses these as the App foreground boundary. ResumeAll keeps
    // output ready even while silent; SuspendAll enables the idle timeout.
    [[nodiscard]] int32_t SuspendAll() override;
    [[nodiscard]] int32_t ResumeAll() override;
};

}  // namespace micropixel::platform::audio

#endif
