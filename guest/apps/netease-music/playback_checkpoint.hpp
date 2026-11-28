// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

#include "sdk/application.hpp"

namespace netease {

struct SavedPlayback final {
    static constexpr uint64_t kMaxId = 9007199254740991ULL;
    uint64_t track{}, account{};
    uint32_t position_ms{}, playing{};
    bool operator==(const SavedPlayback&) const = default;
};
static_assert(sizeof(SavedPlayback) == 24);

class PlaybackCheckpoint final {
   public:
    explicit PlaybackCheckpoint(micropixel::Application& app)
        : app_(app), key_(app.launch_arguments().FindValue("--test-origin") ? "play.test.v1" : "playback.v1") {}

    std::optional<SavedPlayback> Load(uint64_t account) {
        SavedPlayback value{};
        const auto read = app_.storage().GetBytes(key_, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        if (!read || *read != sizeof(value) || !value.track || value.track > SavedPlayback::kMaxId ||
            value.account != account || value.playing > 1)
            return std::nullopt;
        last_ = value;
        return value;
    }

    void Save(SavedPlayback value, bool immediate = false) {
        if (!value.track || value == last_) return;
        const auto now = app_.clock().Now().microseconds();
        const bool transition =
            last_.track != value.track || last_.account != value.account || last_.playing != value.playing;
        if (now < retry_us_ || (!immediate && !transition && now < next_us_)) return;
        if (app_.storage().SetBytes(key_, reinterpret_cast<const uint8_t*>(&value), sizeof(value))) {
            last_ = value;
            next_us_ = now + 15'000'000;
        } else {
            retry_us_ = now + 2'000'000;
        }
    }

    void Clear() {
        (void)app_.storage().Remove(key_);
        last_ = {};
        retry_us_ = next_us_ = 0;
    }

   private:
    micropixel::Application& app_;
    const char* key_;
    SavedPlayback last_{};
    uint64_t retry_us_{}, next_us_{};
};

}  // namespace netease
