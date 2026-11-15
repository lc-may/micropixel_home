// SPDX-License-Identifier: Apache-2.0
#include "sdk/network.hpp"

#include <vector>

#include "runtime/service_binding.hpp"
#include "sdk/audio.hpp"
#include "sdk/input.hpp"
namespace micropixel {
namespace {
using namespace runtime;
ServiceCache http, http_exchange, http_timed, media, text_input;
int32_t HttpCall(uint32_t method, const void* req, uint32_t n, void* out, uint32_t cap, uint32_t& size) {
    auto& cache = method == MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE ? http_timed
                  : method >= MICROPIXEL_NETWORK_METHOD_HTTP_EXCHANGE     ? http_exchange
                                                                          : http;
    auto s = OpenService(cache, MICROPIXEL_SERVICE_NETWORK, 1,
                         method == MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE ? 2
                         : method >= MICROPIXEL_NETWORK_METHOD_HTTP_EXCHANGE     ? 1
                                                                                 : 0);
    return s ? s : CallService(cache, method, req, n, out, cap, size);
}
int32_t MediaCall(uint32_t method, const void* req, uint32_t n, void* out, uint32_t cap, uint32_t& size) {
    auto s = OpenService(media, MICROPIXEL_SERVICE_AUDIO, 1, 1);
    return s ? s : CallService(media, method, req, n, out, cap, size);
}
Result<void> MediaCommand(uint32_t method, uint32_t handle) {
    micropixel_handle_request_t w{sizeof(w), 0, handle};
    uint32_t size = 0;
    auto s = MediaCall(method, &w, sizeof(w), nullptr, 0, size);
    if (s) return unexpected(ErrorFromStatus(s));
    return {};
}
}  // namespace
Result<void> Input::EnableText(bool enabled) const {
    auto s = OpenService(text_input, MICROPIXEL_SERVICE_INPUT, 1, 2);
    uint32_t size = 0;
    micropixel_text_enable_request_t w{sizeof(w), static_cast<uint16_t>(enabled)};
    if (!s) s = CallService(text_input, MICROPIXEL_INPUT_METHOD_TEXT_ENABLE, &w, sizeof(w), nullptr, 0, size);
    if (s) return unexpected(ErrorFromStatus(s));
    return {};
}
Result<FixedString<33>> Input::ReadText() const {
    auto s = OpenService(text_input, MICROPIXEL_SERVICE_INPUT, 1, 2);
    uint32_t size = 0;
    micropixel_text_response_t w{};
    if (!s) s = CallService(text_input, MICROPIXEL_INPUT_METHOD_TEXT_READ, nullptr, 0, &w, sizeof(w), size);
    if (s) return unexpected(ErrorFromStatus(s));
    if (size != sizeof(w) || w.size != sizeof(w) || w.length > 32) return unexpected(Error{ErrorCode::kInternal});
    FixedString<33> result;
    char bytes[33]{};
    CopyBytes(bytes, w.bytes, w.length);
    result.Append(bytes);
    return result;
}
Result<HttpRequest> Network::Start(HttpMethod method, std::string_view url, std::string_view headers,
                                   std::span<const uint8_t> body, uint32_t limit) const {
    return Begin(false, method, url, headers, body, limit);
}
Result<HttpRequest> Network::Exchange(HttpMethod method, std::string_view url, std::string_view headers,
                                      std::span<const uint8_t> body, uint32_t limit, HttpOptions options) const {
    return Begin(true, method, url, headers, body, limit, options);
}
Result<HttpRequest> Network::Begin(bool exchange, HttpMethod method, std::string_view url, std::string_view headers,
                                   std::span<const uint8_t> body, uint32_t limit, HttpOptions options) const {
    const bool timed = exchange && options.receive_timeout_ms != MICROPIXEL_HTTP_DEFAULT_RECEIVE_TIMEOUT_MS;
    if (options.receive_timeout_ms < MICROPIXEL_HTTP_MIN_RECEIVE_TIMEOUT_MS ||
        options.receive_timeout_ms > MICROPIXEL_HTTP_MAX_RECEIVE_TIMEOUT_MS)
        return unexpected(Error{ErrorCode::kInvalidArgument});
    const uint32_t header_bytes =
        timed ? sizeof(micropixel_http_timed_start_request_t) : sizeof(micropixel_http_start_request_t);
    const size_t total = header_bytes + url.size() + headers.size() + body.size();
    if (url.empty() || url.size() > 2048 || headers.size() > 4096 || body.size() > 8192 ||
        total > MICROPIXEL_HTTP_MAX_REQUEST_BYTES || !limit || limit > MICROPIXEL_HTTP_MAX_RESPONSE_BYTES)
        return unexpected(Error{ErrorCode::kInvalidArgument});
    std::vector<uint8_t> bytes(total);
    micropixel_http_start_request_t w{static_cast<uint16_t>(header_bytes), static_cast<uint16_t>(method),
                                      static_cast<uint32_t>(url.size()),   static_cast<uint32_t>(headers.size()),
                                      static_cast<uint32_t>(body.size()),  limit};
    CopyBytes(bytes.data(), &w, sizeof(w));
    if (timed) CopyBytes(bytes.data() + sizeof(w), &options.receive_timeout_ms, sizeof(options.receive_timeout_ms));
    uint32_t offset = header_bytes;
    CopyBytes(bytes.data() + offset, url.data(), url.size());
    offset += url.size();
    if (!headers.empty()) CopyBytes(bytes.data() + offset, headers.data(), headers.size());
    offset += headers.size();
    if (!body.empty()) CopyBytes(bytes.data() + offset, body.data(), body.size());
    micropixel_handle_response_t out{};
    uint32_t size = 0;
    auto s = HttpCall(timed      ? MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE
                      : exchange ? MICROPIXEL_NETWORK_METHOD_HTTP_EXCHANGE
                                 : MICROPIXEL_NETWORK_METHOD_HTTP_START,
                      bytes.data(), bytes.size(), &out, sizeof(out), size);
    if (s) return unexpected(ErrorFromStatus(s));
    if (size != sizeof(out) || out.size != sizeof(out) || !out.handle) return unexpected(Error{ErrorCode::kInternal});
    return HttpRequest(out.handle);
}
Result<HttpState> HttpRequest::State() const {
    micropixel_handle_request_t w{sizeof(w), 0, handle_};
    micropixel_http_state_t out{};
    uint32_t size = 0;
    auto s = HttpCall(MICROPIXEL_NETWORK_METHOD_HTTP_STATE, &w, sizeof(w), &out, sizeof(out), size);
    if (s) return unexpected(ErrorFromStatus(s));
    if (size != sizeof(out) || out.size != sizeof(out) || out.state < 1 || out.state > 3 ||
        out.body_length > MICROPIXEL_HTTP_MAX_RESPONSE_BYTES)
        return unexpected(Error{ErrorCode::kInternal});
    HttpState result{static_cast<HttpPhase>(out.state), out.body_length, out.error};
    CopyBytes(result.message, out.message, 127);
    return result;
}
Result<uint32_t> HttpRequest::Read(uint32_t offset, std::span<uint8_t> destination) const {
    if (destination.size() > MICROPIXEL_HTTP_READ_BYTES) return unexpected(Error{ErrorCode::kInvalidArgument});
    micropixel_http_read_request_t w{sizeof(w), 0, handle_, offset, static_cast<uint32_t>(destination.size())};
    uint32_t size = 0;
    auto s = HttpCall(MICROPIXEL_NETWORK_METHOD_HTTP_READ, &w, sizeof(w), destination.data(), destination.size(), size);
    if (s) return unexpected(ErrorFromStatus(s));
    if (size > destination.size()) return unexpected(Error{ErrorCode::kInternal});
    return size;
}
Result<HttpMetadata> HttpRequest::Metadata() const {
    micropixel_handle_request_t w{sizeof(w), 0, handle_};
    micropixel_http_metadata_t out{};
    uint32_t size = 0;
    auto status = HttpCall(MICROPIXEL_NETWORK_METHOD_HTTP_METADATA, &w, sizeof(w), &out, sizeof(out), size);
    if (status) return unexpected(ErrorFromStatus(status));
    if (size != sizeof(out) || out.size != sizeof(out) || out.headers_length > MICROPIXEL_HTTP_MAX_HEADER_BYTES)
        return unexpected(Error{ErrorCode::kInternal});
    return HttpMetadata{out.status_code, out.headers_length};
}
Result<uint32_t> HttpRequest::ReadHeaders(uint32_t offset, std::span<uint8_t> bytes) const {
    if (bytes.size() > MICROPIXEL_HTTP_READ_BYTES) return unexpected(Error{ErrorCode::kInvalidArgument});
    micropixel_http_read_request_t w{sizeof(w), 0, handle_, offset, static_cast<uint32_t>(bytes.size())};
    uint32_t size = 0;
    auto status = HttpCall(MICROPIXEL_NETWORK_METHOD_HTTP_HEADERS, &w, sizeof(w), bytes.data(), bytes.size(), size);
    if (status) return unexpected(ErrorFromStatus(status));
    if (size > bytes.size()) return unexpected(Error{ErrorCode::kInternal});
    return size;
}
void HttpRequest::Close() {
    if (!handle_) return;
    micropixel_handle_request_t w{sizeof(w), 0, std::exchange(handle_, 0)};
    uint32_t size = 0;
    (void)HttpCall(MICROPIXEL_NETWORK_METHOD_HTTP_CLOSE, &w, sizeof(w), nullptr, 0, size);
}
Result<MediaPlayback> Audio::OpenUrl(std::string_view url) const {
    if (url.empty() || url.size() > MICROPIXEL_MEDIA_MAX_URL_BYTES)
        return unexpected(Error{ErrorCode::kInvalidArgument});
    std::vector<uint8_t> bytes(sizeof(micropixel_media_open_request_t) + url.size());
    micropixel_media_open_request_t w{sizeof(w), 0, static_cast<uint32_t>(url.size())};
    CopyBytes(bytes.data(), &w, sizeof(w));
    CopyBytes(bytes.data() + sizeof(w), url.data(), url.size());
    micropixel_handle_response_t out{};
    uint32_t size = 0;
    auto s = MediaCall(MICROPIXEL_AUDIO_METHOD_MEDIA_OPEN, bytes.data(), bytes.size(), &out, sizeof(out), size);
    if (s) return unexpected(ErrorFromStatus(s));
    if (size != sizeof(out) || out.size != sizeof(out) || !out.handle) return unexpected(Error{ErrorCode::kInternal});
    return MediaPlayback(out.handle);
}
Result<MediaState> MediaPlayback::State() const {
    micropixel_handle_request_t w{sizeof(w), 0, handle_};
    micropixel_media_state_t out{};
    uint32_t size = 0;
    auto s = MediaCall(MICROPIXEL_AUDIO_METHOD_MEDIA_STATE, &w, sizeof(w), &out, sizeof(out), size);
    if (s) return unexpected(ErrorFromStatus(s));
    if (size != sizeof(out) || out.size != sizeof(out) || out.state < 1 || out.state > 5)
        return unexpected(Error{ErrorCode::kInternal});
    MediaState result{static_cast<MediaPhase>(out.state), out.position_ms, out.duration_ms};
    CopyBytes(result.message, out.message, 127);
    return result;
}
Result<void> MediaPlayback::Pause(bool paused) const {
    return MediaCommand(paused ? MICROPIXEL_AUDIO_METHOD_MEDIA_PAUSE : MICROPIXEL_AUDIO_METHOD_MEDIA_RESUME, handle_);
}
Result<void> MediaPlayback::Seek(uint32_t ms) const {
    micropixel_media_seek_request_t w{sizeof(w), 0, handle_, ms};
    uint32_t size = 0;
    auto s = MediaCall(MICROPIXEL_AUDIO_METHOD_MEDIA_SEEK, &w, sizeof(w), nullptr, 0, size);
    if (s) return unexpected(ErrorFromStatus(s));
    return {};
}
void MediaPlayback::Close() {
    if (handle_) (void)MediaCommand(MICROPIXEL_AUDIO_METHOD_MEDIA_CLOSE, std::exchange(handle_, 0));
}
}  // namespace micropixel
