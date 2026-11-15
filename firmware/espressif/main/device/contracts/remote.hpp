// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

#include "abi/micropixel_abi.h"
namespace micropixel::device {
// Platform copies all request data before returning; workers never hold Guest memory.
// Bounded request/media leases per instance. Close cancels without waiting.
class HttpClient {
   public:
    virtual ~HttpClient() = default;
    virtual void SetComponentScope(uint32_t) {}
    virtual uint32_t MaxConcurrentRequests() const { return 1; }
    virtual int32_t Start(uint16_t method, std::string_view url, std::string_view headers,
                          std::span<const uint8_t> body, uint32_t limit, uint32_t& handle) = 0;
    virtual int32_t State(uint32_t handle, micropixel_http_state_t& state) = 0;
    virtual int32_t Read(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& length) = 0;
    virtual int32_t Close(uint32_t handle) = 0;
    virtual int32_t Exchange(uint16_t, std::string_view, std::string_view, std::span<const uint8_t>, uint32_t,
                             uint32_t&) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    virtual int32_t TimedExchange(uint16_t method, std::string_view url, std::string_view headers,
                                  std::span<const uint8_t> body, uint32_t limit, uint32_t timeout_ms,
                                  uint32_t& handle) {
        return timeout_ms == MICROPIXEL_HTTP_DEFAULT_RECEIVE_TIMEOUT_MS
                   ? Exchange(method, url, headers, body, limit, handle)
                   : MICROPIXEL_STATUS_UNSUPPORTED;
    }
    virtual int32_t Metadata(uint32_t, micropixel_http_metadata_t&) { return MICROPIXEL_STATUS_UNSUPPORTED; }
    virtual int32_t Headers(uint32_t, uint32_t, std::span<uint8_t>, uint32_t&) { return MICROPIXEL_STATUS_UNSUPPORTED; }
};
class UrlMedia {
   public:
    virtual ~UrlMedia() = default;
    virtual int32_t Open(std::string_view url, uint32_t& handle) = 0;
    virtual int32_t State(uint32_t handle, micropixel_media_state_t& state) = 0;
    virtual int32_t Pause(uint32_t handle, bool paused) = 0;
    virtual int32_t Seek(uint32_t handle, uint32_t position_ms) = 0;
    virtual int32_t Close(uint32_t handle) = 0;
};
}  // namespace micropixel::device
