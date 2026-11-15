// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <SDL2/SDL.h>

#include <array>
#include <atomic>

#include "device/contracts/audio.hpp"
#include "platform/audio/audio_mixer.hpp"

namespace micropixel::pc {
class SdlAudio final : public device::Audio {
   public:
    SdlAudio() = default;
    ~SdlAudio() override;
    bool Initialize();
    void SetMasterVolume(uint8_t percent);
    uint8_t MasterVolume() const { return master_percent_.load(); }
    int32_t GetInfo(micropixel_audio_info_t&) override;
    int32_t PlayTone(const micropixel_audio_tone_t&) override;
    int32_t StartPcm(const device::PcmSource&, uint32_t, uint16_t, device::PcmStreamHandle&) override;
    // PC Host media only; Guest PcmSource remains mono at the Device boundary.
    int32_t StartStereoPcm(const device::PcmSource&, device::PcmStreamHandle&);
    int32_t PausePcm(device::PcmStreamHandle) override;
    int32_t ResumePcm(device::PcmStreamHandle) override;
    int32_t SetPcmVolume(device::PcmStreamHandle, uint16_t) override;
    int32_t StopPcm(device::PcmStreamHandle) override;
    void BindPcmCompletionSink(device::PcmCompletionSink, void*) override;
    void UnbindPcmCompletionSink(void*) override;
    int32_t StopAll() override;
    int32_t SuspendAll() override;
    int32_t ResumeAll() override;
    uint32_t SampleRate() const { return 48000; }

   private:
    struct Voice {
        device::PcmSource source{};
        uint32_t token{}, generation{};
        uint16_t volume{};
        bool active{}, paused{}, stereo{};
    };
    Voice* Find(device::PcmStreamHandle);
    int32_t Start(const device::PcmSource&, uint32_t, uint16_t, bool stereo, device::PcmStreamHandle&);
    static void Callback(void*, uint8_t*, int);
    void Render(int16_t*, uint32_t);
    SDL_AudioDeviceID device_{};
    std::array<platform::audio::SynthVoice, 8> synth_{};
    std::array<Voice, 4> pcm_{};
    std::array<std::array<int16_t, 256>, 4> pcm_scratch_{};
    int16_t sine_[platform::audio::kSynthSineTableSize]{};
    device::PcmCompletionSink completion_{};
    void* completion_context_{};
    uint16_t master_{10000};
    std::atomic<uint8_t> master_percent_{70};
    bool suspended_{};
};
}  // namespace micropixel::pc
