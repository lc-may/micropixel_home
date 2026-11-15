// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <span>
#include <string_view>
#include <utility>

#include "sdk/result.hpp"
namespace micropixel {
class Application;
enum class HttpMethod : uint16_t { kGet = 1, kPost = 2 };
enum class HttpPhase : uint16_t { kPending = 1, kComplete = 2, kFailed = 3 };
struct HttpState {
    HttpPhase phase{};
    uint32_t body_length{};
    int32_t error{};
    char message[128]{};
};
// Network 1.2: receive idle timeout, 100–60000 ms. Connection/TLS limits are separate.
struct HttpOptions {
    uint32_t receive_timeout_ms{10000};
};
struct HttpMetadata {
    uint16_t status_code{};
    uint32_t headers_length{};
};
class HttpRequest final {
   public:
    HttpRequest() = default;
    HttpRequest(const HttpRequest&) = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;
    HttpRequest(HttpRequest&& other) noexcept : handle_(std::exchange(other.handle_, 0)) {}
    HttpRequest& operator=(HttpRequest&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = std::exchange(other.handle_, 0);
        }
        return *this;
    }
    ~HttpRequest() { Close(); }
    Result<HttpState> State() const;
    Result<HttpMetadata> Metadata() const;
    Result<uint32_t> ReadHeaders(uint32_t offset, std::span<uint8_t> destination) const;
    Result<uint32_t> Read(uint32_t offset, std::span<uint8_t> destination) const;
    void Close();
    bool valid() const { return handle_ != 0; }

   private:
    explicit HttpRequest(uint32_t handle) : handle_(handle) {}
    uint32_t handle_{};
    friend class Network;
};
class Network final {
   public:
    // Header lines: "Name: value\r\n"; GET currently accepts no custom headers.
    // 8 KiB request, 256 KiB response, one request per AppSession.
    Result<HttpRequest> Start(HttpMethod method, std::string_view url, std::string_view headers = {},
                              std::span<const uint8_t> body = {}, uint32_t response_limit = 262144) const;

    // Network 1.1: raw response, custom GET headers, explicit redirects/cookies.
    Result<HttpRequest> Exchange(HttpMethod method, std::string_view url, std::string_view headers = {},
                                 std::span<const uint8_t> body = {}, uint32_t response_limit = 262144,
                                 HttpOptions options = {}) const;

   private:
    Result<HttpRequest> Begin(bool exchange, HttpMethod, std::string_view, std::string_view, std::span<const uint8_t>,
                              uint32_t, HttpOptions options = {}) const;
    struct CapabilityToken {};
    explicit constexpr Network(CapabilityToken) {}
    friend class Application;
};
}  // namespace micropixel
