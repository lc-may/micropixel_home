// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio.hpp"
namespace micropixel::pc {
class MediaPlayer {
   public:
    explicit MediaPlayer(SdlAudio& audio, uint64_t owner = 0);
    ~MediaPlayer();
    void Play(std::string source);
    void Stop();
    void Pause(bool paused);
    bool Paused() const { return paused_.load(); }
    bool Seek(double seconds);
    double PositionSeconds() const { return double(position_.load()) / 48000; }
    double DurationSeconds() const { return double(total_samples_.load()) / 48000; }
    std::string Status();
    bool Finished() const { return finished_.load(); }
    bool Failed() const { return failed_.load(); }
    bool HasBackgroundLease(uint64_t now_ms) const {
        return !Failed() && ((!Finished() && (!Paused() || now_ms - paused_at_ms_.load() < 30000)) ||
                             (ended_at_ms_.load() != 0 && now_ms - ended_at_ms_.load() < 1500));
    }

   private:
    static device::PcmReadResult Read(void*, int16_t*, uint32_t);
    void Run();
    void Load(const std::string&, uint64_t generation, uint32_t component);
    void SetStatus(std::string, uint64_t generation);
    void StopAudioLocked();
    SdlAudio& audio_;
    const uint64_t owner_;
    std::mutex mutex_;
    std::string status_ = "Ready: MP3, WAV and Ogg/Opus";
    std::condition_variable wake_;
    std::optional<std::string> pending_;
    uint64_t generation_{};
    uint32_t pending_component_{};
    std::atomic<uint64_t> paused_at_ms_{};
    std::atomic<uint64_t> ended_at_ms_{};
    bool shutdown_{};
    std::vector<int16_t> samples_;
    std::atomic<size_t> position_{}, total_samples_{};
    device::PcmStreamHandle handle_{};
    std::atomic<bool> cancel_{}, finished_{true}, failed_{}, paused_{};
    std::thread worker_;
};
}  // namespace micropixel::pc
