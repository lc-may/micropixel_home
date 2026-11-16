// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <span>
#include <string_view>

#include "sdk/fixed_string.hpp"
#include "sdk/result.hpp"
namespace micropixel {
enum class AppAction : uint16_t { kSetPlaying = 1, kNextTrack = 2, kRefresh = 3, kOpenPage = 4 };
struct InstanceAddress {
    uint64_t boot{};
    uint32_t slot{}, epoch{}, component{};
};
struct AppMessage {
    uint16_t type{};
    InstanceAddress source{};
    uint64_t sequence{};
    FixedString<68> source_app;
    std::array<uint8_t, 128> payload{};
    uint32_t length{};
    [[nodiscard]] Result<uint32_t> U32() const {
        if (length != 4) return unexpected(Error{ErrorCode::kInvalidArgument});
        return uint32_t(payload[0]) | (uint32_t(payload[1]) << 8) | (uint32_t(payload[2]) << 16) |
               (uint32_t(payload[3]) << 24);
    }
};
struct DeliveryResult {
    bool complete{};
    int32_t status{};
};
class Messages final {
   public:
    [[nodiscard]] Result<void> Allow(std::string_view caller, uint32_t types) const;
    [[nodiscard]] Result<void> AllowColdStart(std::string_view caller, uint32_t types) const;
    // A nonzero request_id retries the same instance-local operation. Reuse
    // identical content; new operations must advance this monotonic ordinal.
    [[nodiscard]] Result<uint32_t> Send(std::string_view app, uint16_t type, std::span<const uint8_t> payload = {},
                                        uint32_t ttl_ms = 5000, uint64_t request_id = 0) const;
    [[nodiscard]] Result<uint32_t> Send(InstanceAddress target, uint16_t type, std::span<const uint8_t> payload = {},
                                        uint32_t ttl_ms = 5000, uint64_t request_id = 0) const;
    [[nodiscard]] Result<AppMessage> Read(uint32_t ticket) const;
    [[nodiscard]] Result<void> Ack(uint32_t ticket, int32_t result = 0) const;
    [[nodiscard]] Result<DeliveryResult> ResultOf(uint32_t ticket) const;
};
}  // namespace micropixel
