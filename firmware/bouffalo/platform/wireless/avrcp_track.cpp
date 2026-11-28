// SPDX-License-Identifier: Apache-2.0
#include "avrcp_track.hpp"

#include <algorithm>

#include "device/text.hpp"

namespace micropixel::bouffalo::avrcp {
namespace {
constexpr uint8_t kGetElementAttributes = 0x20;
constexpr uint8_t kRequestContinuingResponse = 0x40;
constexpr uint8_t kAbortContinuingResponse = 0x41;
constexpr uint8_t kStable = 0x0c;
constexpr uint8_t kSingle = 0;
constexpr uint8_t kStart = 1;
constexpr uint8_t kEnd = 3;
constexpr uint16_t kUtf8 = 106;
constexpr size_t kHeaderBytes = 13;

uint16_t Be16(std::span<const uint8_t> bytes) { return (uint16_t(bytes[0]) << 8) | bytes[1]; }

void CopyText(std::span<const uint8_t> input, std::span<char> output) {
    auto count = std::min(input.size(), output.size() - 1);
    while (count && count < input.size() && (input[count] & 0xc0) == 0x80) --count;
    if (!device::IsValidUtf8(input.data(), static_cast<uint32_t>(count))) return;
    for (size_t index = 0; index < count; ++index) {
        const auto byte = input[index];
        output[index] = byte < 0x20 || byte == 0x7f ? ' ' : static_cast<char>(byte);
    }
    output[count] = '\0';
}

bool Parse(std::span<const uint8_t> data, micropixel_bluetooth_remote_track_t& track) {
    if (data.empty()) return false;
    const size_t count = data.front();
    data = data.subspan(1);
    const std::array<std::span<char>, 3> text{track.title, track.artist, track.album};
    for (size_t attribute = 0; attribute < count; ++attribute) {
        if (data.size() < 8) return false;
        const auto length = Be16(data.subspan(6));
        const auto id = data[3];
        const auto charset = Be16(data.subspan(4));
        const bool known = data[0] == 0 && data[1] == 0 && data[2] == 0 && id >= 1 && id <= text.size();
        data = data.subspan(8);
        if (length > data.size()) return false;
        if (known && charset == kUtf8) CopyText(data.first(length), text[id - 1]);
        data = data.subspan(length);
    }
    return data.empty();
}
}  // namespace

std::span<const uint8_t> TrackCommand(TrackAction action) {
    // AV/C ctype, panel subunit, vendor opcode, SIG company ID, PDU ID,
    // fragment type, parameter length, then command parameters.
    constexpr static uint8_t read[]{
        1, 0x48, 0,  0, 0x19, 0x58, kGetElementAttributes,
        0, 0,    21, 0, 0,    0,    0,
        0, 0,    0,  0, 3,  // Current track identifier, attributes 1–3.
        0, 0,    0,  1, 0,    0,    0,
        2, 0,    0,  0, 3,
    };
    constexpr static uint8_t next[]{
        0, 0x48, 0, 0, 0x19, 0x58, kRequestContinuingResponse, 0, 0, 1, kGetElementAttributes};
    constexpr static uint8_t abort[]{
        0, 0x48, 0, 0, 0x19, 0x58, kAbortContinuingResponse, 0, 0, 1, kGetElementAttributes};
    switch (action) {
        case TrackAction::kRead:
            return read;
        case TrackAction::kContinue:
            return next;
        case TrackAction::kAbort:
            return abort;
        default:
            return {};
    }
}

bool IsTrackResponse(std::span<const uint8_t> packet) {
    return packet.size() >= 10 && (packet[0] & 0x0f) == 2 && packet[1] == 0x11 && packet[2] == 0x0e &&
           packet[4] == 0x48 && packet[5] == 0 && packet[6] == 0 && packet[7] == 0x19 && packet[8] == 0x58 &&
           (packet[9] == kGetElementAttributes || packet[9] == kRequestContinuingResponse ||
            packet[9] == kAbortContinuingResponse);
}

void TrackResponse::Reset() {
    size_ = 0;
    request_ = next_ = TrackAction::kNone;
}

void TrackResponse::Expect(TrackAction action, uint8_t label) {
    request_ = action == TrackAction::kAbort ? TrackAction::kNone : action;
    next_ = TrackAction::kNone;
    label_ = label;
}

void TrackResponse::Expire() {
    const bool abort = request_ == TrackAction::kContinue;
    Reset();
    if (abort) next_ = TrackAction::kAbort;
}

TrackResult TrackResponse::Accept(std::span<const uint8_t> packet, micropixel_bluetooth_remote_track_t& track) {
    if (!waiting() || !IsTrackResponse(packet) || (packet[0] >> 4) != label_ || packet[9] == kAbortContinuingResponse ||
        (packet[9] == kRequestContinuingResponse && request_ != TrackAction::kContinue))
        return TrackResult::kIgnored;

    const bool first = request_ == TrackAction::kRead;
    if (packet.size() < kHeaderBytes || packet[9] != kGetElementAttributes || (packet[3] & 0x0f) != kStable) {
        Expire();
        return TrackResult::kUnavailable;
    }
    const auto fragment = packet[10] & 3;
    const auto bytes = packet.subspan(kHeaderBytes);
    if (bytes.empty() || bytes.size() != Be16(packet.subspan(11)) || bytes.size() > data_.size() - size_ ||
        first != (fragment <= kStart)) {
        Reset();
        if (fragment == kStart || fragment == 2) next_ = TrackAction::kAbort;
        return TrackResult::kUnavailable;
    }
    std::copy(bytes.begin(), bytes.end(), data_.begin() + size_);
    size_ += bytes.size();
    if (fragment != kSingle && fragment != kEnd) {
        request_ = TrackAction::kNone;
        next_ = TrackAction::kContinue;
        return TrackResult::kIncomplete;
    }
    const bool valid = Parse(std::span{data_}.first(size_), track);
    Reset();
    return valid ? TrackResult::kComplete : TrackResult::kUnavailable;
}
}  // namespace micropixel::bouffalo::avrcp
