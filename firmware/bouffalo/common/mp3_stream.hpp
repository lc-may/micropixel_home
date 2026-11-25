// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace micropixel::bouffalo {

struct Mp3Frame final {
    uint32_t bytes{}, rate{}, samples{}, bitrate_kbps{};

    uint32_t Duration(uint64_t frames) const {
        return rate ? std::min<uint64_t>(UINT32_MAX, frames * samples * 1000U / rate) : 0;
    }
};

inline Mp3Frame ParseMp3Frame(const uint8_t* p) {
    const unsigned version = (p[1] >> 3) & 3;
    const unsigned bitrate_index = p[2] >> 4;
    const unsigned rate_index = (p[2] >> 2) & 3;
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0 || (p[1] & 6) != 2 || version == 1 || !bitrate_index ||
        bitrate_index == 15 || rate_index == 3)
        return {};
    constexpr uint16_t kMpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    constexpr uint16_t kMpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    constexpr uint32_t kRates[] = {44100, 48000, 32000};
    const uint32_t rate = kRates[rate_index] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    const uint32_t bitrate = version == 3 ? kMpeg1[bitrate_index] : kMpeg2[bitrate_index];
    const uint32_t length = (version == 3 ? 144000U : 72000U) * bitrate / rate;
    return {length + ((p[2] >> 1) & 1), rate, version == 3 ? 1152U : 576U, bitrate};
}

inline uint32_t Mp3ReadBe32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

inline uint32_t Mp3TaggedFrameCount(std::span<const uint8_t> bytes, const Mp3Frame& frame) {
    const bool mono = (bytes[3] >> 6) == 3;
    const size_t xing = 4 + (frame.samples == 1152 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (bytes.size() >= xing + 12 &&
        (std::memcmp(bytes.data() + xing, "Xing", 4) == 0 || std::memcmp(bytes.data() + xing, "Info", 4) == 0) &&
        (Mp3ReadBe32(bytes.data() + xing + 4) & 1U))
        return Mp3ReadBe32(bytes.data() + xing + 8);
    constexpr size_t kVbri = 4 + 32;
    if (bytes.size() >= kVbri + 18 && std::memcmp(bytes.data() + kVbri, "VBRI\0\1", 6) == 0)
        return Mp3ReadBe32(bytes.data() + kVbri + 14);
    return 0;
}

class Mp3Stream final {
   public:
    static constexpr size_t kBufferSize = 2048;

    void Reset() {
        buffered_ = skip_ = prefix_ = searched_ = frames_ = 0;
        format_ = {};
        checked_id3_ = trailer_ = false;
    }

    template <typename Sink>
    bool Push(std::span<const uint8_t> bytes, Sink&& sink) {
        while (!bytes.empty() && !trailer_) {
            if (skip_) {
                const auto skipped = std::min<size_t>(skip_, bytes.size());
                bytes = bytes.subspan(skipped);
                skip_ -= skipped;
                if (bytes.empty()) break;
            }
            const auto copied = std::min(bytes.size(), buffer_.size() - buffered_);
            std::memcpy(buffer_.data() + buffered_, bytes.data(), copied);
            bytes = bytes.subspan(copied);
            std::span<const uint8_t> staged{buffer_.data(), buffered_ + copied};
            if (!Consume(staged, sink)) return false;
            buffered_ = staged.size();
            if (buffered_) std::memmove(buffer_.data(), staged.data(), buffered_);
        }
        return true;
    }

    bool Complete() const { return frames_ >= 2 && (trailer_ || buffered_ < 4); }
    uint32_t PrefixBytes() const { return prefix_; }
    uint32_t Duration() const { return format_.Duration(frames_); }

   private:
    static bool Compatible(const Mp3Frame& left, const Mp3Frame& right) {
        return left.rate == right.rate && left.samples == right.samples;
    }

    template <typename Sink>
    bool Consume(std::span<const uint8_t>& bytes, Sink& sink) {
        if (!checked_id3_) {
            if (bytes.size() < 10) return true;
            checked_id3_ = true;
            if (std::memcmp(bytes.data(), "ID3", 3) == 0) {
                uint32_t tag = 0;
                for (unsigned i = 6; i < 10; ++i) {
                    if (bytes[i] & 0x80) return false;
                    tag = (tag << 7) | bytes[i];
                }
                prefix_ = tag + (bytes[3] == 4 && (bytes[5] & 0x10) ? 20 : 10);
                const auto staged_skip = std::min<size_t>(prefix_, bytes.size());
                skip_ = prefix_ - staged_skip;
                bytes = bytes.subspan(staged_skip);
            }
        }
        while (bytes.size() >= 4) {
            const auto frame = ParseMp3Frame(bytes.data());
            if (!format_.rate) {
                if (frame.bytes && bytes.size() < frame.bytes + 4) break;
                if (!frame.bytes || !Compatible(frame, ParseMp3Frame(bytes.data() + frame.bytes))) {
                    if (++searched_ >= 4096U) return false;
                    ++prefix_;
                    bytes = bytes.subspan(1);
                    continue;
                }
                format_ = frame;
            }
            if (!Compatible(format_, frame)) {
                // The first non-frame after the aligned MPEG stream is trailing metadata.
                trailer_ = true;
                bytes = {};
                break;
            }
            if (bytes.size() < frame.bytes) break;
            if (!sink(bytes.first(frame.bytes), frame, frames_)) return false;
            ++frames_;
            bytes = bytes.subspan(frame.bytes);
        }
        return true;
    }

    std::array<uint8_t, kBufferSize> buffer_{};
    size_t buffered_{};
    uint32_t skip_{}, prefix_{}, searched_{}, frames_{};
    Mp3Frame format_{};
    bool checked_id3_{}, trailer_{};
};

}  // namespace micropixel::bouffalo
