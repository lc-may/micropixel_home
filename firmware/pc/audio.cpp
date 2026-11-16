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
int32_t SdlAudio::PlayTone(const micropixel_audio_tone_t& tone) { return Tone(tone, 0); }
int32_t SdlAudio::Tone(const micropixel_audio_tone_t& tone, uint64_t owner) {
    if (!device_) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (!platform::audio::AudioMixer::ValidTone(tone)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    AudioLock lock(device_);
    platform::audio::SynthVoice* selected = nullptr;
    for (auto& v : synth_) {
        if (!v.active) {
            selected = &v;
            break;
        }
        if (synth_owners_[&v - synth_.data()] == owner &&
            (!selected || v.remaining_frames < selected->remaining_frames))
            selected = &v;
    }
    if (!selected) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    synth_owners_[selected - synth_.data()] = owner;
    auto* binding = FindBinding(owner);
    synth_components_[selected - synth_.data()] = binding ? binding->component : 0;
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
uint32_t SdlAudio::ComponentScope(uint64_t owner) {
    AudioLock lock(device_);
    const auto* binding = FindBinding(owner);
    return binding ? binding->component : 0;
}
int32_t SdlAudio::StartStereoPcm(const device::PcmSource& src, device::PcmStreamHandle& out, uint64_t owner,
                                 uint32_t component) {
    return Start(src, 0, 1000, true, out, owner, component);
}
int32_t SdlAudio::Start(const device::PcmSource& src, uint32_t token, uint16_t volume, bool stereo,
                        device::PcmStreamHandle& out, uint64_t owner, uint32_t component) {
    out = 0;
    if (!device_) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (!src.read || !src.context || volume > 1000) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    AudioLock lock(device_);
    for (size_t i = 0; i < pcm_.size(); ++i) {
        auto& v = pcm_[i];
        if (v.active) continue;
        v.generation = (v.generation + 1) & 0xffffffU;
        if (!v.generation) ++v.generation;
        v.owner = owner;
        auto* binding = FindBinding(owner);
        v.component = component == UINT32_MAX ? (binding ? binding->component : 0) : component;
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
void SdlAudio::SetAlarmFocus(bool active) {
    SDL_LockAudioDevice(device_);
    if (active && !alarm_active_.load()) alarm_frame_ = 0;
    alarm_active_ = active;
    SDL_UnlockAudioDevice(device_);
}
uint64_t SdlAudio::RenderedFrames(uint64_t owner) {
    SDL_LockAudioDevice(device_);
    const auto* binding = FindBinding(owner);
    auto result = binding ? binding->rendered_frames : 0;
    SDL_UnlockAudioDevice(device_);
    return result;
}
void SdlAudio::Render(int16_t* output, uint32_t frames) {
    if (suspended_) return;
    constexpr uint32_t chunk = 128;
    for (uint32_t offset = 0; offset < frames; offset += chunk) {
        const auto count = std::min(chunk, frames - offset);
        uint32_t counts[4]{};
        device::PcmCompletion completed[4]{};
        uint64_t completed_owners[4]{};
        uint32_t completions = 0;
        for (size_t i = 0; i < pcm_.size(); ++i) {
            auto& v = pcm_[i];
            auto* binding = FindBinding(v.owner);
            if (!v.active || v.paused || (binding && binding->suspended)) continue;
            const auto result = v.source.read(v.source.context, pcm_scratch_[i].data(), count);
            counts[i] = std::min(result.frames, count);
            if (binding) binding->rendered_frames += counts[i];
            if (result.finished || result.frames > count) {
                completed_owners[completions] = v.owner;
                completed[completions++] = {v.token,
                                            result.frames > count ? MICROPIXEL_STATUS_INTERNAL : MICROPIXEL_STATUS_OK};
                v.active = false;
            }
        }
        for (uint32_t f = 0; f < count; ++f) {
            int32_t mix = 0;
            for (size_t i = 0; i < synth_.size(); ++i) {
                auto* binding = FindBinding(synth_owners_[i]);
                if (synth_[i].active && (!binding || !binding->suspended))
                    mix += platform::audio::AudioMixer::NextSample(synth_[i], sine_);
            }
            int32_t left = mix, right = mix;
            for (size_t i = 0; i < pcm_.size(); ++i) {
                if (f >= counts[i]) continue;
                const auto& voice = pcm_[i];
                left += static_cast<int32_t>(pcm_scratch_[i][voice.stereo ? 2 * f : f]) * voice.volume / 1000;
                right += static_cast<int32_t>(pcm_scratch_[i][voice.stereo ? 2 * f + 1 : f]) * voice.volume / 1000;
            }
            if (alarm_active_.load()) {
                left /= 5;
                right /= 5;
                const auto frame = alarm_frame_++;
                const int32_t tone = frame % 48000 < 32000
                                         ? sine_[(frame * 880 * platform::audio::kSynthSineTableSize / 48000) %
                                                 platform::audio::kSynthSineTableSize] *
                                               3500 / 32768
                                         : 0;
                left += tone;
                right += tone;
            }
            output[(offset + f) * 2] = std::clamp<int64_t>(static_cast<int64_t>(left) * master_ / 10000, -32768, 32767);
            output[(offset + f) * 2 + 1] =
                std::clamp<int64_t>(static_cast<int64_t>(right) * master_ / 10000, -32768, 32767);
        }
        for (uint32_t i = 0; i < completions; ++i) {
            auto* binding = FindBinding(completed_owners[i]);
            if (binding && binding->sink)
                binding->sink(binding->context, completed[i]);
            else if (!completed_owners[i] && completion_)
                completion_(completion_context_, completed[i]);
        }
    }
}
SdlAudio::Binding* SdlAudio::FindBinding(uint64_t owner) {
    if (!owner) return nullptr;
    for (auto& binding : bindings_)
        if (binding.owner == owner) return &binding;
    return nullptr;
}
ScopedAudio::ScopedAudio(SdlAudio& audio, uint64_t owner) : audio_(audio), owner_(owner) {
    AudioLock lock(audio_.device_);
    if (!owner || audio_.FindBinding(owner)) return;
    for (auto& binding : audio_.bindings_)
        if (!binding.owner) {
            binding.owner = owner;
            bound_ = true;
            break;
        }
}
ScopedAudio::~ScopedAudio() {
    if (!bound_) return;
    (void)StopAll();
    AudioLock lock(audio_.device_);
    if (auto* binding = audio_.FindBinding(owner_)) *binding = {};
}
int32_t ScopedAudio::StartPcm(const device::PcmSource& source, uint32_t token, uint16_t volume,
                              device::PcmStreamHandle& handle) {
    if (!bound_) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    return audio_.Start(source, token, volume, false, handle, owner_);
}
int32_t ScopedAudio::PausePcm(device::PcmStreamHandle handle) {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    AudioLock lock(audio_.device_);
    auto* voice = audio_.Find(handle);
    if (!voice || voice->owner != owner_) return MICROPIXEL_STATUS_NOT_FOUND;
    voice->paused = true;
    return MICROPIXEL_STATUS_OK;
}
int32_t ScopedAudio::ResumePcm(device::PcmStreamHandle handle) {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    AudioLock lock(audio_.device_);
    auto* voice = audio_.Find(handle);
    if (!voice || voice->owner != owner_) return MICROPIXEL_STATUS_NOT_FOUND;
    voice->paused = false;
    return MICROPIXEL_STATUS_OK;
}
int32_t ScopedAudio::SetPcmVolume(device::PcmStreamHandle handle, uint16_t volume) {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    if (volume > 1000) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    AudioLock lock(audio_.device_);
    auto* voice = audio_.Find(handle);
    if (!voice || voice->owner != owner_) return MICROPIXEL_STATUS_NOT_FOUND;
    voice->volume = volume;
    return MICROPIXEL_STATUS_OK;
}
int32_t ScopedAudio::StopPcm(device::PcmStreamHandle handle) {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    AudioLock lock(audio_.device_);
    auto* voice = audio_.Find(handle);
    if (!voice || voice->owner != owner_) return MICROPIXEL_STATUS_NOT_FOUND;
    voice->active = false;
    voice->source = {};
    return MICROPIXEL_STATUS_OK;
}
void ScopedAudio::BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) {
    if (!bound_) return;
    AudioLock lock(audio_.device_);
    if (auto* binding = audio_.FindBinding(owner_)) {
        binding->sink = sink;
        binding->context = context;
    }
}
void ScopedAudio::UnbindPcmCompletionSink(void* context) {
    if (!bound_) return;
    AudioLock lock(audio_.device_);
    if (auto* binding = audio_.FindBinding(owner_); binding && binding->context == context) {
        binding->sink = nullptr;
        binding->context = nullptr;
    }
}
int32_t ScopedAudio::StopAll() {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    AudioLock lock(audio_.device_);
    for (size_t i = 0; i < audio_.synth_.size(); ++i)
        if (audio_.synth_owners_[i] == owner_) audio_.synth_[i].active = false;
    for (auto& voice : audio_.pcm_)
        if (voice.owner == owner_) {
            voice.active = false;
            voice.source = {};
        }
    return MICROPIXEL_STATUS_OK;
}
int32_t ScopedAudio::SuspendAll() {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    AudioLock lock(audio_.device_);
    if (auto* binding = audio_.FindBinding(owner_)) binding->suspended = true;
    return MICROPIXEL_STATUS_OK;
}
int32_t ScopedAudio::ResumeAll() {
    if (!bound_) return MICROPIXEL_STATUS_NOT_FOUND;
    AudioLock lock(audio_.device_);
    if (auto* binding = audio_.FindBinding(owner_)) binding->suspended = false;
    return MICROPIXEL_STATUS_OK;
}
void ScopedAudio::SetComponentScope(uint32_t token) {
    if (!bound_) return;
    AudioLock lock(audio_.device_);
    if (auto* binding = audio_.FindBinding(owner_)) binding->component = token;
}
void ScopedAudio::StopComponent(uint32_t token) {
    if (!bound_) return;
    AudioLock lock(audio_.device_);
    for (size_t i = 0; i < audio_.synth_.size(); ++i)
        if (audio_.synth_owners_[i] == owner_ && audio_.synth_components_[i] == token) audio_.synth_[i].active = false;
    for (auto& voice : audio_.pcm_)
        if (voice.owner == owner_ && voice.component == token) {
            voice.active = false;
            voice.source = {};
        }
}
}  // namespace micropixel::pc
