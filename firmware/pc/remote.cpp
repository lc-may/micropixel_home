// SPDX-License-Identifier: Apache-2.0
#include "remote.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace micropixel::pc {
HttpChannel::HttpChannel() : worker_(&HttpChannel::Run, this) {}
HttpChannel::~HttpChannel() {
    {
        std::lock_guard lock(mutex_);
        shutdown_ = true;
        cancel_ = true;
        pending_.reset();
    }
    wake_.notify_one();
    worker_.join();
}
int32_t HttpChannel::Start(uint16_t method, std::string_view url, std::string_view headers,
                           std::span<const uint8_t> body, uint32_t limit, uint32_t& handle) {
    return Begin(false, method, url, headers, body, limit, handle);
}
int32_t HttpChannel::Exchange(uint16_t method, std::string_view url, std::string_view headers,
                              std::span<const uint8_t> body, uint32_t limit, uint32_t& handle) {
    return Begin(true, method, url, headers, body, limit, handle);
}
int32_t HttpChannel::TimedExchange(uint16_t method, std::string_view url, std::string_view headers,
                                   std::span<const uint8_t> body, uint32_t limit, uint32_t timeout_ms,
                                   uint32_t& handle) {
    return Begin(true, method, url, headers, body, limit, handle, timeout_ms);
}
int32_t HttpChannel::Begin(bool exchange, uint16_t method, std::string_view url, std::string_view headers,
                           std::span<const uint8_t> body, uint32_t limit, uint32_t& handle,
                           uint32_t receive_timeout_ms) {
    if (receive_timeout_ms < 100 || receive_timeout_ms > 60000 || url.empty() || url.size() > 2048 ||
        headers.size() > 4096 || body.size() > 8192 || !limit || limit > MICROPIXEL_HTTP_MAX_RESPONSE_BYTES ||
        (method != MICROPIXEL_HTTP_GET && method != MICROPIXEL_HTTP_POST))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    Job job;
    job.method = method;
    job.exchange = exchange;
    job.url = url;
    if (!body.empty()) job.body.assign(reinterpret_cast<const char*>(body.data()), body.size());
    job.limit = limit;
    job.receive_timeout_ms = receive_timeout_ms;
    Url parsed;
    std::string error;
    if (!ParseUrl(job.url, parsed, error) || (parsed.scheme != "http" && parsed.scheme != "https"))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    while (!headers.empty()) {
        auto end = headers.find("\r\n");
        if (end == std::string_view::npos || job.headers.size() == 8) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        auto line = headers.substr(0, end);
        auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 || line.find('\0') != std::string_view::npos ||
            line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        auto value = line.substr(colon + 1);
        while (value.starts_with(' ')) value.remove_prefix(1);
        job.headers.emplace_back(line.substr(0, colon), value);
        headers.remove_prefix(end + 2);
    }
    std::lock_guard lock(mutex_);
    if (handle_) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    if (++generation_ == 0 || generation_ > 0x7fffffffU) generation_ = 1;
    handle = handle_ = job.handle = generation_;
    body_.clear();
    headers_.clear();
    metadata_ = {sizeof(metadata_), 0, 0};
    state_ = {};
    state_.size = sizeof(state_);
    state_.state = MICROPIXEL_HTTP_PENDING;
    pending_ = std::move(job);
    wake_.notify_one();
    return MICROPIXEL_STATUS_OK;
}
void HttpChannel::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return shutdown_ || pending_.has_value(); });
            if (shutdown_) return;
            job = std::move(*pending_);
            pending_.reset();
            cancel_ = false;
        }
        std::vector<uint8_t> result;
        std::string error;
        HttpResponse exchange;
        bool ok = job.exchange ? HttpExchange(job.method == MICROPIXEL_HTTP_POST, job.url, job.body, job.headers,
                                              exchange, error, &cancel_, job.limit, job.receive_timeout_ms)
                  : job.method == MICROPIXEL_HTTP_GET
                      ? HttpGet(job.url, result, error, &cancel_, job.limit)
                      : HttpPost(job.url, job.body, job.headers, result, error, &cancel_, job.limit);
        if (job.exchange && ok) result = std::move(exchange.body);
        std::lock_guard lock(mutex_);
        if (handle_ != job.handle) continue;
        state_.state = ok ? MICROPIXEL_HTTP_COMPLETE : MICROPIXEL_HTTP_FAILED;
        state_.error = ok ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
        std::snprintf(state_.message, sizeof(state_.message), "%s", error.c_str());
        if (ok) {
            headers_ = std::move(exchange.headers);
            metadata_ = {sizeof(metadata_), exchange.status, static_cast<uint32_t>(headers_.size())};
            body_ = std::move(result);
            state_.body_length = body_.size();
        }
    }
}
int32_t HttpChannel::State(uint32_t handle, micropixel_http_state_t& state) {
    std::lock_guard lock(mutex_);
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    state = state_;
    return MICROPIXEL_STATUS_OK;
}
int32_t HttpChannel::Read(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& length) {
    std::lock_guard lock(mutex_);
    length = 0;
    if (!handle || handle != handle_ || bytes.size() > MICROPIXEL_HTTP_READ_BYTES || offset > body_.size())
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (state_.state != MICROPIXEL_HTTP_COMPLETE) return MICROPIXEL_STATUS_WOULD_BLOCK;
    length = std::min(bytes.size(), body_.size() - offset);
    if (length) std::memcpy(bytes.data(), body_.data() + offset, length);
    return MICROPIXEL_STATUS_OK;
}
int32_t HttpChannel::Metadata(uint32_t handle, micropixel_http_metadata_t& metadata) {
    std::lock_guard lock(mutex_);
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (state_.state != MICROPIXEL_HTTP_COMPLETE) return MICROPIXEL_STATUS_WOULD_BLOCK;
    metadata = metadata_;
    return MICROPIXEL_STATUS_OK;
}
int32_t HttpChannel::Headers(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& length) {
    std::lock_guard lock(mutex_);
    length = 0;
    if (!handle || handle != handle_ || offset > headers_.size() || bytes.size() > MICROPIXEL_HTTP_READ_BYTES)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (state_.state != MICROPIXEL_HTTP_COMPLETE) return MICROPIXEL_STATUS_WOULD_BLOCK;
    length = std::min(bytes.size(), headers_.size() - offset);
    if (length) std::memcpy(bytes.data(), headers_.data() + offset, length);
    return MICROPIXEL_STATUS_OK;
}
int32_t HttpChannel::Close(uint32_t handle) {
    std::lock_guard lock(mutex_);
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    handle_ = 0;
    cancel_ = true;
    pending_.reset();
    headers_.clear();
    body_.clear();
    return MICROPIXEL_STATUS_OK;
}
int32_t GuestHttp::Start(uint16_t method, std::string_view url, std::string_view headers, std::span<const uint8_t> body,
                         uint32_t limit, uint32_t& handle) {
    return Begin(0, method, url, headers, body, limit, 10000, handle);
}
int32_t GuestHttp::Exchange(uint16_t method, std::string_view url, std::string_view headers,
                            std::span<const uint8_t> body, uint32_t limit, uint32_t& handle) {
    return Begin(1, method, url, headers, body, limit, 10000, handle);
}
int32_t GuestHttp::TimedExchange(uint16_t method, std::string_view url, std::string_view headers,
                                 std::span<const uint8_t> body, uint32_t limit, uint32_t timeout, uint32_t& handle) {
    return Begin(2, method, url, headers, body, limit, timeout, handle);
}
int32_t GuestHttp::Begin(unsigned mode, uint16_t method, std::string_view url, std::string_view headers,
                         std::span<const uint8_t> body, uint32_t limit, uint32_t timeout, uint32_t& handle) {
    const unsigned lane = (component_ & 3) == MICROPIXEL_COMPONENT_SERVICE ? 1 : 0;
    uint32_t inner = 0;
    const auto status = mode == 2   ? lanes_[lane].TimedExchange(method, url, headers, body, limit, timeout, inner)
                        : mode == 1 ? lanes_[lane].Exchange(method, url, headers, body, limit, inner)
                                    : lanes_[lane].Start(method, url, headers, body, limit, inner);
    handle = status ? 0 : (inner << 1) | lane;
    if (!status) active_[lane] = true;
    return status;
}
int32_t GuestHttp::Metadata(uint32_t handle, micropixel_http_metadata_t& out) {
    return lanes_[handle & 1].Metadata(handle >> 1, out);
}
int32_t GuestHttp::Headers(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& size) {
    return lanes_[handle & 1].Headers(handle >> 1, offset, bytes, size);
}
int32_t GuestHttp::State(uint32_t handle, micropixel_http_state_t& out) {
    return lanes_[handle & 1].State(handle >> 1, out);
}
int32_t GuestHttp::Read(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& size) {
    return lanes_[handle & 1].Read(handle >> 1, offset, bytes, size);
}
int32_t GuestHttp::Close(uint32_t handle) {
    auto status = lanes_[handle & 1].Close(handle >> 1);
    if (!status) active_[handle & 1] = false;
    return status;
}
int32_t GuestMedia::Open(std::string_view url, uint32_t& handle) {
    if (handle_) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    Url parsed;
    std::string error;
    if (url.size() > 2048 || !ParseUrl(std::string(url), parsed, error) ||
        (parsed.scheme != "http" && parsed.scheme != "https"))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (++generation_ == 0) ++generation_;
    handle = handle_ = generation_;
    player_.Play(std::string(url));
    return MICROPIXEL_STATUS_OK;
}
int32_t GuestMedia::State(uint32_t handle, micropixel_media_state_t& state) {
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    state = {};
    state.size = sizeof(state);
    state.state = player_.Failed()                ? MICROPIXEL_MEDIA_FAILED
                  : player_.Paused()              ? MICROPIXEL_MEDIA_PAUSED
                  : player_.Finished()            ? MICROPIXEL_MEDIA_FINISHED
                  : player_.DurationSeconds() > 0 ? MICROPIXEL_MEDIA_PLAYING
                                                  : MICROPIXEL_MEDIA_LOADING;
    state.position_ms = player_.PositionSeconds() * 1000;
    state.duration_ms = player_.DurationSeconds() * 1000;
    std::snprintf(state.message, sizeof(state.message), "%s", player_.Status().c_str());
    return MICROPIXEL_STATUS_OK;
}
int32_t GuestMedia::Pause(uint32_t handle, bool paused) {
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    player_.Pause(paused);
    return MICROPIXEL_STATUS_OK;
}
int32_t GuestMedia::Seek(uint32_t handle, uint32_t ms) {
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    return player_.Seek(double(ms) / 1000) ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_WOULD_BLOCK;
}
int32_t GuestMedia::Close(uint32_t handle) {
    if (!handle || handle != handle_) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    player_.Stop();
    handle_ = 0;
    return MICROPIXEL_STATUS_OK;
}
}  // namespace micropixel::pc
