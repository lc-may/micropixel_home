// SPDX-License-Identifier: Apache-2.0
#include "url_media_endpoint.hpp"

#include <cstring>
#include <string_view>

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
int32_t UrlMediaEndpoint::Call(uint32_t method, const uint8_t* req, uint32_t size, uint8_t* out, uint32_t capacity,
                               uint32_t& length) {
    length = 0;
    if (!backend_) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_OPEN) {
        micropixel_media_open_request_t w{};
        if (!req || size < sizeof(w)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        std::memcpy(&w, req, sizeof(w));
        if (w.size != sizeof(w) || w.reserved0 || !w.url_length || w.url_length > MICROPIXEL_MEDIA_MAX_URL_BYTES ||
            uint64_t(w.url_length) + sizeof(w) != size)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const std::string_view url(reinterpret_cast<const char*>(req + sizeof(w)), w.url_length);
        if (!Text(url) || (!url.starts_with("https://") && !url.starts_with("http://")))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (!out || capacity < sizeof(micropixel_handle_response_t)) {
            length = sizeof(micropixel_handle_response_t);
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        if (handle_) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        auto s = backend_->Open(url, handle_);
        if (s) {
            handle_ = 0;
            return s;
        }
        return Write(micropixel_handle_response_t{sizeof(micropixel_handle_response_t), 0, handle_}, out, capacity,
                     length);
    }
    uint32_t handle = 0;
    micropixel_media_seek_request_t seek{};
    if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_SEEK) {
        if (!Read(req, size, seek)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        handle = seek.handle;
    } else {
        micropixel_handle_request_t w{};
        if (!Read(req, size, w)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        handle = w.handle;
    }
    if (!handle_ || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_STATE) {
        micropixel_media_state_t state{};
        auto s = backend_->State(handle_, state);
        return s ? s : Write(state, out, capacity, length);
    }
    if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_PAUSE || method == MICROPIXEL_AUDIO_METHOD_MEDIA_RESUME)
        return backend_->Pause(handle_, method == MICROPIXEL_AUDIO_METHOD_MEDIA_PAUSE);
    if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_SEEK) return backend_->Seek(handle_, seek.position_ms);
    if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_CLOSE) {
        Close();
        return MICROPIXEL_STATUS_OK;
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

}  // namespace micropixel::runtime
