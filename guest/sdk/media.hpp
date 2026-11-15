// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <utility>

#include "sdk/result.hpp"
namespace micropixel {
class Audio;
enum class MediaPhase : uint16_t { kLoading = 1, kPlaying = 2, kPaused = 3, kFinished = 4, kFailed = 5 };
struct MediaState {
    MediaPhase phase{};
    uint32_t position_ms{}, duration_ms{};
    char message[128]{};
};
// Move-only URL stream. Destruction/Close stops playback and cancels pending work.
class MediaPlayback final {
   public:
    MediaPlayback() = default;
    MediaPlayback(const MediaPlayback&) = delete;
    MediaPlayback& operator=(const MediaPlayback&) = delete;
    MediaPlayback(MediaPlayback&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    MediaPlayback& operator=(MediaPlayback&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~MediaPlayback() { Close(); }
    Result<MediaState> State() const;
    Result<void> Pause(bool paused) const;
    Result<void> Seek(uint32_t position_ms) const;
    void Close();
    bool valid() const { return handle_ != 0; }

   private:
    explicit MediaPlayback(uint32_t handle) : handle_(handle) {}
    uint32_t handle_{};
    friend class Audio;
};
}  // namespace micropixel
