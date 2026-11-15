// SPDX-License-Identifier: Apache-2.0
#include "media.hpp"

#include <mpg123.h>
#include <opusfile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_timer.h"
#include "network.hpp"
#include "portable_app.hpp"

namespace micropixel::pc {
MediaPlayer::MediaPlayer(SdlAudio& audio, uint64_t owner) : audio_(audio), owner_(owner), worker_([this] { Run(); }) {}
MediaPlayer::~MediaPlayer() {
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        cancel_ = true;
        pending_.reset();
        StopAudioLocked();
    }
    wake_.notify_one();
    worker_.join();
}
void MediaPlayer::SetStatus(std::string status, uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (generation == generation_ && !cancel_) status_ = std::move(status);
}
std::string MediaPlayer::Status() {
    std::lock_guard lock(mutex_);
    return status_;
}
void MediaPlayer::Play(std::string source) {
    {
        std::lock_guard lock(mutex_);
        cancel_ = true;
        StopAudioLocked();
        pending_ = std::move(source);
        pending_component_ = audio_.ComponentScope(owner_);
        ended_at_ms_ = 0;
        ++generation_;
        finished_ = false;
        failed_ = false;
        paused_ = false;
        position_ = total_samples_ = 0;
        status_ = "Loading audio...";
    }
    wake_.notify_one();
}
void MediaPlayer::StopAudioLocked() {
    if (handle_) (void)audio_.StopPcm(handle_);
    handle_ = 0;
}
void MediaPlayer::Stop() {
    std::lock_guard lock(mutex_);
    cancel_ = true;
    ended_at_ms_ = 0;
    pending_.reset();
    ++generation_;
    StopAudioLocked();
    position_ = total_samples_ = 0;
    paused_ = false;
    finished_ = true;
    failed_ = false;
    status_ = "Stopped";
}
void MediaPlayer::Pause(bool paused) {
    std::lock_guard lock(mutex_);
    if (paused) paused_at_ms_ = esp_timer_get_time() / 1000;
    paused_ = paused;
    if (handle_) {
        if (paused)
            (void)audio_.PausePcm(handle_);
        else
            (void)audio_.ResumePcm(handle_);
    }
}
bool MediaPlayer::Seek(double seconds) {
    std::lock_guard lock(mutex_);
    if (!handle_ || finished_ || !std::isfinite(seconds) || seconds < 0 || total_samples_ == 0) return false;
    const auto sample = std::min(seconds, double(total_samples_.load()) / 48000) * 48000;
    position_ = std::min(static_cast<size_t>(sample), total_samples_.load() - 1);
    return true;
}
void MediaPlayer::Run() {
    for (;;) {
        std::string source;
        uint64_t generation;
        uint32_t component;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return shutdown_ || pending_.has_value(); });
            if (shutdown_) return;
            source = std::move(*pending_);
            pending_.reset();
            generation = generation_;
            component = pending_component_;
            cancel_ = false;
            StopAudioLocked();
        }
        samples_.clear();
        Load(source, generation, component);
    }
}
device::PcmReadResult MediaPlayer::Read(void* context, int16_t* out, uint32_t capacity) {
    auto& player = *static_cast<MediaPlayer*>(context);
    size_t position = player.position_.load();
    const uint32_t count = std::min<size_t>(capacity, player.samples_.size() / 2 - position);
    std::copy_n(player.samples_.data() + position * 2, count * 2, out);
    const bool advanced = player.position_.compare_exchange_strong(position, position + count);
    const bool finished = advanced && position + count == player.samples_.size() / 2;
    if (finished) {
        player.ended_at_ms_ = esp_timer_get_time() / 1000;
        player.finished_ = true;
    }
    return {count, finished};
}
void MediaPlayer::Load(const std::string& source, uint64_t generation, uint32_t component) {
    auto fail = [this, generation](std::string error) {
        std::lock_guard lock(mutex_);
        if (cancel_ || generation != generation_) return;
        std::fprintf(stderr, "Audio: %s\n", error.c_str());
        status_ = std::move(error);
        failed_ = true;
        finished_ = true;
    };
    std::vector<uint8_t> bytes;
    std::string error;
    if (source.starts_with("http://") || source.starts_with("https://")) {
        if (!HttpGet(source, bytes, error, &cancel_)) {
            fail(error);
            return;
        }
    } else if (!ReadFile(source, bytes, 32U * 1024 * 1024)) {
        fail("Cannot read local audio file");
        return;
    }
    if (cancel_) {
        return;
    }
    SetStatus("Decoding audio...", generation);
    constexpr size_t maximum_samples = 64U * 1024 * 1024;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
        int status = 0;
        std::unique_ptr<OggOpusFile, decltype(&op_free)> file(op_open_memory(bytes.data(), bytes.size(), &status),
                                                              op_free);
        if (!file) {
            fail("Invalid Ogg/Opus file");
            return;
        }
        auto stereo = std::make_unique<std::array<int16_t, 4096>>();
        while (!cancel_) {
            const int frames = op_read_stereo(file.get(), stereo->data(), stereo->size());
            if (frames == 0) break;
            if (frames < 0 || samples_.size() + frames * 2 > maximum_samples) {
                fail("Corrupt or oversized Ogg/Opus audio");
                return;
            }
            samples_.insert(samples_.end(), stereo->begin(), stereo->begin() + frames * 2);
        }
    } else if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
               std::memcmp(bytes.data() + 8, "WAVE", 4) == 0) {
        SDL_AudioSpec spec{};
        uint8_t* data = nullptr;
        uint32_t length = 0;
        if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(bytes.data(), bytes.size()), 1, &spec, &data, &length)) {
            fail("Invalid WAV audio");
            return;
        }
        std::unique_ptr<uint8_t, decltype(&SDL_FreeWAV)> wav(data, SDL_FreeWAV);
        SDL_AudioCVT converter{};
        const int status =
            SDL_BuildAudioCVT(&converter, spec.format, spec.channels, spec.freq, AUDIO_S16SYS, 2, audio_.SampleRate());
        if (status < 0 || uint64_t(length) * converter.len_mult > maximum_samples * sizeof(int16_t)) {
            fail("Cannot convert audio format or file is too large");
            return;
        }
        std::vector<uint8_t> converted(uint64_t(length) * converter.len_mult);
        std::memcpy(converted.data(), data, length);
        converter.buf = converted.data();
        converter.len = length;
        if (status > 0 && SDL_ConvertAudio(&converter) != 0) {
            fail(SDL_GetError());
            return;
        }
        const size_t count = (status > 0 ? converter.len_cvt : length) / sizeof(int16_t);
        samples_.resize(count);
        std::memcpy(samples_.data(), converted.data(), count * sizeof(int16_t));
    } else {
        static std::once_flag initialized;
        static int library_status = MPG123_ERR;
        std::call_once(initialized, [] { library_status = mpg123_init(); });
        if (library_status != MPG123_OK) {
            fail("Cannot initialize MP3 decoder");
            return;
        }
        int status = 0;
        std::unique_ptr<mpg123_handle, decltype(&mpg123_delete)> decoder(mpg123_new(nullptr, &status), mpg123_delete);
        if (!decoder ||
            mpg123_param(decoder.get(), MPG123_ADD_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0) != MPG123_OK ||
            mpg123_param(decoder.get(), MPG123_FORCE_RATE, 48000, 0) != MPG123_OK ||
            mpg123_format_none(decoder.get()) != MPG123_OK ||
            mpg123_format(decoder.get(), 48000, MPG123_STEREO, MPG123_ENC_SIGNED_16) != MPG123_OK ||
            mpg123_open_feed(decoder.get()) != MPG123_OK ||
            mpg123_feed(decoder.get(), bytes.data(), bytes.size()) != MPG123_OK) {
            fail("Cannot initialize MP3 file");
            return;
        }
        auto block = std::make_unique<std::array<int16_t, 4096>>();
        while (!cancel_) {
            size_t decoded = 0;
            status = mpg123_read(decoder.get(), block->data(), sizeof(*block), &decoded);
            if (status == MPG123_NEW_FORMAT) {
                long rate = 0;
                int channels = 0, encoding = 0;
                mpg123_getformat(decoder.get(), &rate, &channels, &encoding);
                if (rate != 48000 || channels != 2 || encoding != MPG123_ENC_SIGNED_16) {
                    fail("Unsupported MP3 output format");
                    return;
                }
            } else if (status != MPG123_OK && status != MPG123_DONE && status != MPG123_NEED_MORE) {
                fail("Corrupt or unsupported MP3 audio");
                return;
            }
            if (decoded > sizeof(*block) || decoded % 4 || samples_.size() + decoded / 2 > maximum_samples) {
                fail("Decoded audio exceeds 128 MiB");
                return;
            }
            samples_.insert(samples_.end(), block->begin(), block->begin() + decoded / 2);
            if (status == MPG123_DONE || status == MPG123_NEED_MORE) break;
        }
    }
    if (cancel_) {
        return;
    }
    if (samples_.empty()) {
        fail("Audio file contains no samples");
        return;
    }
    std::lock_guard lock(mutex_);
    if (cancel_ || generation != generation_) return;
    position_ = 0;
    total_samples_ = samples_.size() / 2;
    (void)audio_.ResumeAll();
    const auto status = audio_.StartStereoPcm({Read, this}, handle_, owner_, component);
    if (status != MICROPIXEL_STATUS_OK) {
        status_ = "No audio playback slot available";
        failed_ = true;
        finished_ = true;
        return;
    }
    if (paused_) (void)audio_.PausePcm(handle_);
    status_ = "Playing audio";
    std::fprintf(stderr, "Audio playing: %zu frames at 48000 Hz stereo\n", samples_.size() / 2);
}
}  // namespace micropixel::pc
