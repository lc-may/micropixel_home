// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <span>

#include "abi/bluetooth.h"

namespace micropixel::bouffalo::avrcp {

enum class TrackAction : uint8_t { kNone, kRead, kContinue, kAbort };
enum class TrackResult : uint8_t { kIgnored, kIncomplete, kComplete, kUnavailable };

// Commands exclude the AVCTP header, which the pinned BT stack adds.
std::span<const uint8_t> TrackCommand(TrackAction action);
bool IsTrackResponse(std::span<const uint8_t> packet);

class TrackResponse final {
   public:
    void Reset();
    void Expect(TrackAction action, uint8_t label);
    void Expire();
    bool waiting() const { return request_ != TrackAction::kNone; }
    TrackAction next() const { return next_; }
    TrackResult Accept(std::span<const uint8_t> packet, micropixel_bluetooth_remote_track_t& track);

   private:
    // AVRCP control replies may be split at any byte. Keep the wire payload
    // separate until all fields have been validated and can be published.
    std::array<uint8_t, 4096> data_{};
    size_t size_{};
    TrackAction request_{};
    TrackAction next_{};
    uint8_t label_{};
};
}  // namespace micropixel::bouffalo::avrcp
