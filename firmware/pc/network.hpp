// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace micropixel::pc {
struct Url {
    std::string scheme, host, port, path;
    bool tls{};
};
bool ParseUrl(const std::string&, Url&, std::string& error);
class SocketStream {
   public:
    SocketStream();
    ~SocketStream();
    bool Connect(const Url&, std::atomic<bool>* cancel, std::string& error);
    int32_t Read(void*, size_t);
    int32_t ReadWithTimeout(void*, size_t, uint32_t receive_timeout_ms);
    int32_t Write(const void*, size_t);

   private:
    struct State;
    std::unique_ptr<State> state_;
};
bool HttpGet(const std::string& url, std::vector<uint8_t>& body, std::string& error,
             std::atomic<bool>* cancel = nullptr, size_t limit = 32U * 1024 * 1024);
using HttpHeader = std::pair<std::string, std::string>;
struct HttpResponse {
    uint16_t status{};
    std::string headers;
    std::vector<uint8_t> body;
};
bool HttpExchange(bool post, const std::string& url, const std::string& payload, std::span<const HttpHeader> headers,
                  HttpResponse& response, std::string& error, std::atomic<bool>* cancel = nullptr,
                  size_t limit = 262144, uint32_t receive_timeout_ms = 10000);
bool HttpPost(const std::string& url, const std::string& payload, std::span<const HttpHeader> headers,
              std::vector<uint8_t>& body, std::string& error, std::atomic<bool>* cancel = nullptr,
              size_t limit = 2U * 1024 * 1024);
bool MqttRoundTrip(const std::string& url, const std::string& topic, const std::string& payload, std::string& received,
                   std::string& error, std::atomic<bool>* cancel = nullptr);
}  // namespace micropixel::pc
