// SPDX-License-Identifier: Apache-2.0
#include "audio.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "platform/audio/volume_curve.hpp"

namespace micropixel::pc {
namespace {
struct AudioLock {
    SDL_AudioDeviceID device;
    explicit AudioLock(SDL_AudioDeviceID id) : device(id) {
        if (device) SDL_LockAudioDevice(device);
    }
    ~AudioLock() {
        if (device) SDL_UnlockAudioDevice(device);
    }
};
}  // namespace
bool SdlAudio::Initialize() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec desired{}, obtained{};
    desired.freq = SampleRate();
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    desired.callback = Callback;
    desired.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (!device_) return false;
    platform::audio::AudioMixer::InitializeSineTable(sine_);
    SetMasterVolume(70);
    SDL_PauseAudioDevice(device_, 0);
    std::fprintf(stderr, "SDL audio: driver=%s, 48000 Hz stereo S16\n", SDL_GetCurrentAudioDriver());
    return true;
}
SdlAudio::~SdlAudio() {
    if (device_) SDL_CloseAudioDevice(device_);
}
void SdlAudio::SetMasterVolume(uint8_t percent) {
    AudioLock lock(device_);
    percent = std::min<uint8_t>(percent, 100);
    master_percent_ = percent;
    master_ = platform::audio::VolumeOutputPerTenThousand(percent);
}
int32_t SdlAudio::GetInfo(micropixel_audio_info_t& info) {
    if (!device_) return MICROPIXEL_STATUS_UNSUPPORTED;
    info = {.size = sizeof(info),
            .max_voices = 8,
            .sample_rate = SampleRate(),
            .capabilities = MICROPIXEL_AUDIO_CAPABILITY_OGG_OPUS | MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM,
            .supported_waveforms = (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4),
            .max_tone_duration_ms = 5000,
            .max_clips = 16,
            .max_playbacks = 2,
            .max_pcm_streams = 1};
    return MICROPIXEL_STATUS_OK;
}
int32_t SdlAudio::PlayTone(const micropixel_audio_tone_t& tone) {
    if (!device_) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (!platform::audio::AudioMixer::ValidTone(tone)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    AudioLock lock(device_);
    auto* selected = &synth_[0];
    for (auto& v : synth_) {
        if (!v.active) {
            selected = &v;
            break;
        }
        if (v.remaining_frames < selected->remaining_frames) selected = &v;
    }
    platform::audio::AudioMixer::StartVoice(*selected, tone, SampleRate());
    return MICROPIXEL_STATUS_OK;
}
SdlAudio::Voice* SdlAudio::Find(device::PcmStreamHandle h) {
    const auto index = h & 255U;
    if (!index || index > pcm_.size()) return nullptr;
    auto& v = pcm_[index - 1];
    return v.active && v.generation == h >> 8 ? &v : nullptr;
}
int32_t SdlAudio::StartPcm(const device::PcmSource& src, uint32_t token, uint16_t volume,
                           device::PcmStreamHandle& out) {
    return Start(src, token, volume, false, out);
}
int32_t SdlAudio::StartStereoPcm(const device::PcmSource& src, device::PcmStreamHandle& out) {
    return Start(src, 0, 1000, true, out);
}
int32_t SdlAudio::Start(const device::PcmSource& src, uint32_t token, uint16_t volume, bool stereo,
                        device::PcmStreamHandle& out) {
    out = 0;
    if (!device_) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (!src.read || !src.context || volume > 1000) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    AudioLock lock(device_);
    for (size_t i = 0; i < pcm_.size(); ++i) {
        auto& v = pcm_[i];
        if (v.active) continue;
        v.generation = (v.generation + 1) & 0xffffffU;
        if (!v.generation) ++v.generation;
        v.source = src;
        v.token = token;
        v.volume = volume;
        v.paused = false;
        v.stereo = stereo;
        v.active = true;
        out = (v.generation << 8) | (i + 1);
        return MICROPIXEL_STATUS_OK;
    }
    return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
}
int32_t SdlAudio::PausePcm(device::PcmStreamHandle h) {
    AudioLock lock(device_);
    auto* v = Find(h);
    if (!v) return MICROPIXEL_STATUS_NOT_FOUND;
    v->paused = true;
    return MICROPIXEL_STATUS_OK;
}
int32_t SdlAudio::ResumePcm(device::PcmStreamHandle h) {
    AudioLock lock(device_);
    auto* v = Find(h);
    if (!v) return MICROPIXEL_STATUS_NOT_FOUND;
    v->paused = false;
    return MICROPIXEL_STATUS_OK;
}
int32_t SdlAudio::SetPcmVolume(device::PcmStreamHandle h, uint16_t volume) {
    if (volume > 1000) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    AudioLock lock(device_);
    auto* v = Find(h);
    if (!v) return MICROPIXEL_STATUS_NOT_FOUND;
    v->volume = volume;
    return MICROPIXEL_STATUS_OK;
}
int32_t SdlAudio::StopPcm(device::PcmStreamHandle h) {
    AudioLock lock(device_);
    auto* v = Find(h);
    if (!v) return MICROPIXEL_STATUS_NOT_FOUND;
    v->active = false;
    v->source = {};
    return MICROPIXEL_STATUS_OK;
}
void SdlAudio::BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) {
    AudioLock lock(device_);
    completion_ = sink;
    completion_context_ = context;
}
void SdlAudio::UnbindPcmCompletionSink(void* context) {
    AudioLock lock(device_);
    if (completion_context_ == context) {
        completion_ = nullptr;
        completion_context_ = nullptr;
    }
}
int32_t SdlAudio::StopAll() {
    AudioLock lock(device_);
    for (auto& v : synth_) v.active = false;
    for (auto& v : pcm_) v.active = false;
    return MICROPIXEL_STATUS_OK;
}
int32_t SdlAudio::SuspendAll() {
    AudioLock lock(device_);
    suspended_ = true;
    return MICROPIXEL_STATUS_OK;
}
int32_t SdlAudio::ResumeAll() {
    AudioLock lock(device_);
    suspended_ = false;
    return MICROPIXEL_STATUS_OK;
}
void SdlAudio::Callback(void* ctx, uint8_t* bytes, int count) {
    std::memset(bytes, 0, count);
    static_cast<SdlAudio*>(ctx)->Render(reinterpret_cast<int16_t*>(bytes), count / (2 * sizeof(int16_t)));
}
void SdlAudio::Render(int16_t* output, uint32_t frames) {
    if (suspended_) return;
    constexpr uint32_t chunk = 128;
    for (uint32_t offset = 0; offset < frames; offset += chunk) {
        const auto count = std::min(chunk, frames - offset);
        uint32_t counts[4]{};
        device::PcmCompletion completed[4]{};
        uint32_t completions = 0;
        for (size_t i = 0; i < pcm_.size(); ++i) {
            auto& v = pcm_[i];
            if (!v.active || v.paused) continue;
            const auto result = v.source.read(v.source.context, pcm_scratch_[i].data(), count);
            counts[i] = std::min(result.frames, count);
            if (result.finished || result.frames > count) {
                completed[completions++] = {v.token,
                                            result.frames > count ? MICROPIXEL_STATUS_INTERNAL : MICROPIXEL_STATUS_OK};
                v.active = false;
            }
        }
        for (uint32_t f = 0; f < count; ++f) {
            int32_t mix = 0;
            for (auto& v : synth_)
                if (v.active) mix += platform::audio::AudioMixer::NextSample(v, sine_);
            int32_t left = mix, right = mix;
            for (size_t i = 0; i < pcm_.size(); ++i) {
                if (f >= counts[i]) continue;
                const auto& voice = pcm_[i];
                left += static_cast<int32_t>(pcm_scratch_[i][voice.stereo ? 2 * f : f]) * voice.volume / 1000;
                right += static_cast<int32_t>(pcm_scratch_[i][voice.stereo ? 2 * f + 1 : f]) * voice.volume / 1000;
            }
            output[(offset + f) * 2] = std::clamp<int64_t>(static_cast<int64_t>(left) * master_ / 10000, -32768, 32767);
            output[(offset + f) * 2 + 1] =
                std::clamp<int64_t>(static_cast<int64_t>(right) * master_ / 10000, -32768, 32767);
        }
        if (completion_)
            for (uint32_t i = 0; i < completions; ++i) completion_(completion_context_, completed[i]);
    }
}
}  // namespace micropixel::pc
