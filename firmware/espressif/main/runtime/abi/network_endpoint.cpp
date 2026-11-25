// SPDX-License-Identifier: Apache-2.0
#include "network_endpoint.hpp"

#include <algorithm>
#include <cstring>

namespace micropixel::runtime {
namespace {
template <class T>
bool Read(const uint8_t* p, uint32_t n, T& out) {
    if (!p || n != sizeof(T)) return false;
    std::memcpy(&out, p, sizeof(T));
    if (out.size != n) return false;
    if constexpr (requires { out.reserved0; })
        if (out.reserved0) return false;
    return true;
}
template <class T>
int32_t Write(const T& value, uint8_t* p, uint32_t n, uint32_t& size) {
    size = sizeof(T);
    if (!p || n < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(p, &value, size);
    return MICROPIXEL_STATUS_OK;
}
bool Text(std::string_view s) {
    for (unsigned char c : s)
        if (c == 0 || c == '\r' || c == '\n') return false;
    return true;
}
}  // namespace
ServiceDescriptor NetworkServiceEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_NETWORK,
            .interface_major = 1,
            .interface_minor = static_cast<uint16_t>(backend_ && backend_->MaxConcurrentRequests() >= 2 ? 3 : 2),
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .capabilities = backend_ ? 1U : 0U,
            .max_request_bytes = MICROPIXEL_HTTP_MAX_REQUEST_BYTES,
            .max_response_bytes = MICROPIXEL_HTTP_READ_BYTES};
}
int32_t NetworkServiceEndpoint::Call(uint32_t method, const uint8_t* req, uint32_t size, uint8_t* out,
                                     uint32_t capacity, uint32_t& length) {
    length = 0;
    if (!backend_) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (method == MICROPIXEL_NETWORK_METHOD_HTTP_START || method == MICROPIXEL_NETWORK_METHOD_HTTP_EXCHANGE ||
        method == MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE) {
        micropixel_http_start_request_t w{};
        if (!req || size < sizeof(w) || size > MICROPIXEL_HTTP_MAX_REQUEST_BYTES)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        std::memcpy(&w, req, sizeof(w));
        const bool timed = method == MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE;
        const uint32_t header_bytes = timed ? sizeof(micropixel_http_timed_start_request_t) : sizeof(w);
        uint32_t receive_timeout_ms = MICROPIXEL_HTTP_DEFAULT_RECEIVE_TIMEOUT_MS;
        if (size < header_bytes) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (timed) std::memcpy(&receive_timeout_ms, req + sizeof(w), sizeof(receive_timeout_ms));
        if (receive_timeout_ms < MICROPIXEL_HTTP_MIN_RECEIVE_TIMEOUT_MS ||
            receive_timeout_ms > MICROPIXEL_HTTP_MAX_RECEIVE_TIMEOUT_MS)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const uint64_t total = uint64_t(header_bytes) + w.url_length + w.headers_length + w.body_length;
        if (w.size != header_bytes || total != size || !w.url_length || w.url_length > 2048 ||
            w.headers_length > 4096 || !w.response_limit || w.response_limit > MICROPIXEL_HTTP_MAX_RESPONSE_BYTES ||
            (w.method != MICROPIXEL_HTTP_GET && w.method != MICROPIXEL_HTTP_POST) ||
            (w.method == MICROPIXEL_HTTP_GET &&
             (w.body_length || (method == MICROPIXEL_NETWORK_METHOD_HTTP_START && w.headers_length))))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (!out || capacity < sizeof(micropixel_handle_response_t)) {
            length = sizeof(micropixel_handle_response_t);
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        auto* lease = std::find(handles_.begin(), handles_.end(), 0U);
        if (lease == handles_.end()) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        uint32_t handle_ = 0;
        const std::string_view url(reinterpret_cast<const char*>(req + header_bytes), w.url_length);
        const std::string_view headers(reinterpret_cast<const char*>(req + header_bytes + w.url_length),
                                       w.headers_length);
        if (!Text(url) || (!url.starts_with("https://") && !url.starts_with("http://")))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const std::span<const uint8_t> body{req + header_bytes + w.url_length + w.headers_length, w.body_length};
        auto status =
            timed ? backend_->TimedExchange(w.method, url, headers, body, w.response_limit, receive_timeout_ms, handle_)
            : method == MICROPIXEL_NETWORK_METHOD_HTTP_EXCHANGE
                ? backend_->Exchange(w.method, url, headers, body, w.response_limit, handle_)
                : backend_->Start(w.method, url, headers, body, w.response_limit, handle_);
        if (status) {
            handle_ = 0;
            return status;
        }
        *lease = handle_;
        return Write(micropixel_handle_response_t{sizeof(micropixel_handle_response_t), 0, handle_}, out, capacity,
                     length);
    }
    if (method == MICROPIXEL_NETWORK_METHOD_HTTP_READ || method == MICROPIXEL_NETWORK_METHOD_HTTP_HEADERS) {
        micropixel_http_read_request_t w{};
        if (!Read(req, size, w) || !w.handle ||
            std::find(handles_.begin(), handles_.end(), w.handle) == handles_.end() ||
            w.capacity > MICROPIXEL_HTTP_READ_BYTES)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (capacity < w.capacity || (!out && w.capacity)) {
            length = w.capacity;
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        return method == MICROPIXEL_NETWORK_METHOD_HTTP_HEADERS
                   ? backend_->Headers(w.handle, w.offset, {out, w.capacity}, length)
                   : backend_->Read(w.handle, w.offset, {out, w.capacity}, length);
    }
    micropixel_handle_request_t w{};
    if (!Read(req, size, w) || !w.handle || std::find(handles_.begin(), handles_.end(), w.handle) == handles_.end())
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (method == MICROPIXEL_NETWORK_METHOD_HTTP_METADATA) {
        micropixel_http_metadata_t meta{};
        auto status = backend_->Metadata(w.handle, meta);
        return status ? status : Write(meta, out, capacity, length);
    }
    if (method == MICROPIXEL_NETWORK_METHOD_HTTP_STATE) {
        micropixel_http_state_t state{};
        auto s = backend_->State(w.handle, state);
        return s ? s : Write(state, out, capacity, length);
    }
    if (method == MICROPIXEL_NETWORK_METHOD_HTTP_CLOSE) {
        auto s = backend_->Close(w.handle);
        if (!s) *std::find(handles_.begin(), handles_.end(), w.handle) = 0;
        return s;
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}
}  // namespace micropixel::runtime
