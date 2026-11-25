// SPDX-License-Identifier: Apache-2.0
#include "network.hpp"

#include <strings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#include "background_task.hpp"
#include "device/contracts/remote.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/controller/app_manager.hpp"
#include "https_get.h"
#include "network_time.h"
#include "wireless.h"
#include "work/task_policy.hpp"

namespace micropixel::bouffalo {
namespace {

constexpr uint32_t kLaneCount = 2;
constexpr uint32_t kHttpTaskStackBytes = 16U * 1024U;
constexpr uint32_t kUrlCapacity = 2049;
std::array<BackgroundTask, kLaneCount> workers{};

struct PsramFree {
    void operator()(uint8_t* memory) const { heap_caps_free(memory); }
};

class Request final {
   public:
    enum class Phase : uint8_t { kIdle, kPending, kComplete, kFailed };

    Phase GetPhase() const { return phase_.load(std::memory_order_acquire); }

    int32_t Begin(uint16_t method, std::string_view url, std::string_view headers, std::span<const uint8_t> body,
                  uint32_t limit, uint32_t timeout, bool exchange) {
        memory_.reset();
        const auto bytes = kUrlCapacity + headers.size() + body.size() + MICROPIXEL_HTTP_MAX_HEADER_BYTES + limit;
        memory_.reset(static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)));
        if (!memory_) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        char* address = reinterpret_cast<char*>(memory_.get());
        std::memcpy(address, url.data(), url.size());
        address[url.size()] = '\0';
        auto* extra = memory_.get() + kUrlCapacity;
        if (!headers.empty()) std::memcpy(extra, headers.data(), headers.size());
        auto* payload = extra + headers.size();
        if (!body.empty()) std::memcpy(payload, body.data(), body.size());
        request_ = {.url = address,
                    .headers = reinterpret_cast<const char*>(extra),
                    .headers_length = headers.size(),
                    .body = payload,
                    .body_length = body.size(),
                    .receive_timeout_ms = timeout,
                    .post = method == MICROPIXEL_HTTP_POST};
        if (!mpx_https_valid_request(&request_)) {
            memory_.reset();
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        auto* response_headers = payload + body.size();
        response_ = {.context = this,
                     .cancelled = Cancelled,
                     .body = response_headers + MICROPIXEL_HTTP_MAX_HEADER_BYTES,
                     .capacity = limit,
                     .headers = reinterpret_cast<char*>(response_headers),
                     .headers_capacity = MICROPIXEL_HTTP_MAX_HEADER_BYTES};
        exchange_ = exchange;
        error_ = 0;
        cancelled_.store(false, std::memory_order_relaxed);
        phase_.store(Phase::kPending, std::memory_order_release);
        return MICROPIXEL_STATUS_OK;
    }

    int32_t State(micropixel_http_state_t& state) const {
        const auto phase = GetPhase();
        state.size = sizeof(state);
        state.state = phase == Phase::kPending    ? MICROPIXEL_HTTP_PENDING
                      : phase == Phase::kComplete ? MICROPIXEL_HTTP_COMPLETE
                                                  : MICROPIXEL_HTTP_FAILED;
        if (phase == Phase::kComplete) state.body_length = response_.length;
        if (phase == Phase::kFailed) {
            state.error = error_;
            std::snprintf(state.message, sizeof(state.message), "%s (%ld)",
                          error_ == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED ? "HTTPS response exceeds size limit"
                          : error_ == MICROPIXEL_STATUS_TIMEOUT          ? "HTTPS timeout"
                                                                         : "HTTPS request failed",
                          static_cast<long>(error_detail_));
        }
        return MICROPIXEL_STATUS_OK;
    }

    int32_t Metadata(micropixel_http_metadata_t& metadata) const {
        if (GetPhase() != Phase::kComplete) return MICROPIXEL_STATUS_WOULD_BLOCK;
        metadata = {
            .size = sizeof(metadata), .status_code = response_.status, .headers_length = response_.headers_length};
        return MICROPIXEL_STATUS_OK;
    }

    int32_t Read(bool headers, uint32_t offset, std::span<uint8_t> bytes, uint32_t& length) const {
        if (GetPhase() != Phase::kComplete) return MICROPIXEL_STATUS_WOULD_BLOCK;
        const auto size = headers ? response_.headers_length : response_.length;
        const auto* data = headers ? reinterpret_cast<const uint8_t*>(response_.headers) : response_.body;
        if (offset > size) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        length = std::min<uint32_t>(bytes.size(), size - offset);
        if (length) std::memcpy(bytes.data(), data + offset, length);
        return MICROPIXEL_STATUS_OK;
    }

    void Close() {
        cancelled_.store(true, std::memory_order_relaxed);
        if (GetPhase() != Phase::kPending) memory_.reset();
    }

    void RunPending() {
        if (GetPhase() != Phase::kPending) return;
        int result = -ECANCELED;
        for (unsigned attempt = 0; !Cancelled(this); ++attempt) {
            response_.length = response_.headers_length = response_.status = 0;
            result = mpx_https_exchange(&request_, &response_);
            if (result || exchange_ || (response_.status >= 200 && response_.status < 300)) break;
            if (!request_.post && response_.status >= 300 && response_.status < 400 && attempt < 3 && Redirect())
                continue;
            result = -EPROTO;
            break;
        }
        error_detail_ = result == -EPROTO ? response_.status : result;
        error_ = response_.overflow     ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED
                 : result == -ETIMEDOUT ? MICROPIXEL_STATUS_TIMEOUT
                 : result == -ECANCELED ? MICROPIXEL_STATUS_CANCELLED
                                        : MICROPIXEL_STATUS_INTERNAL;
        const bool complete = !result && !response_.overflow && !Cancelled(this);
        if (cancelled_.load(std::memory_order_relaxed)) memory_.reset();
        phase_.store(complete ? Phase::kComplete : Phase::kFailed, std::memory_order_release);
    }

   private:
    bool Redirect() {
        std::string_view remaining(response_.headers, response_.headers_length);
        while (!remaining.empty()) {
            const auto end = remaining.find("\r\n");
            if (end == remaining.npos) return false;
            const auto line = remaining.substr(0, end);
            remaining.remove_prefix(end + 2);
            constexpr std::string_view key = "Location: ";
            if (line.size() <= key.size() || strncasecmp(line.data(), key.data(), key.size())) continue;
            auto location = line.substr(key.size());
            while (location.starts_with(' ') || location.starts_with('\t')) location.remove_prefix(1);
            while (location.ends_with(' ') || location.ends_with('\t')) location.remove_suffix(1);
            const std::string_view url(request_.url);
            size_t prefix = 0;
            if (location.starts_with("//")) {
                prefix = sizeof("https:") - 1;
            } else if (location.starts_with('/')) {
                prefix = std::min(url.find_first_of("/?", 8), url.size());
            } else if (!location.starts_with("https://")) {
                if (location.empty() || location.find(':') != location.npos) return false;
                const auto authority = std::min(url.find_first_of("/?", 8), url.size());
                const auto path = url.substr(0, url.find('?'));
                const auto directory = path.rfind('/');
                prefix = directory < authority ? authority : directory + 1;
                if (prefix == authority) {
                    if (prefix + 1 >= kUrlCapacity) return false;
                    memory_.get()[prefix++] = '/';
                }
            }
            if (prefix + location.size() >= kUrlCapacity) return false;
            std::memcpy(memory_.get() + prefix, location.data(), location.size());
            memory_.get()[prefix + location.size()] = '\0';
            // Credentials and other caller headers belong to the original request.
            request_.headers_length = 0;
            return mpx_https_valid_request(&request_);
        }
        return false;
    }

    static bool Cancelled(void* context) {
        return static_cast<Request*>(context)->cancelled_.load(std::memory_order_relaxed) || !mpx_wifi_online();
    }

    std::atomic<Phase> phase_{Phase::kIdle};
    std::atomic<bool> cancelled_{};
    std::unique_ptr<uint8_t, PsramFree> memory_;
    mpx_https_request_t request_{};
    mpx_https_response_t response_{};
    bool exchange_{};
    int32_t error_{}, error_detail_{};
};

struct Lane final {
    // A replacement can be queued while the outgoing request unwinds.
    std::array<Request, 2> requests;
    Request* active{};
    uint32_t generation{}, handle{};
};

class HttpSession final : public device::HttpClient {
   public:
    uint32_t MaxConcurrentRequests() const override { return kLaneCount; }
    void SetComponentScope(uint32_t token) override { lane_ = (token & 3) == MICROPIXEL_COMPONENT_SERVICE; }

    int32_t Start(uint16_t method, std::string_view url, std::string_view headers, std::span<const uint8_t> body,
                  uint32_t limit, uint32_t& handle) override {
        return Begin(method, url, headers, body, limit, MICROPIXEL_HTTP_DEFAULT_RECEIVE_TIMEOUT_MS, false, handle);
    }

    int32_t Exchange(uint16_t method, std::string_view url, std::string_view headers, std::span<const uint8_t> body,
                     uint32_t limit, uint32_t& handle) override {
        return TimedExchange(method, url, headers, body, limit, MICROPIXEL_HTTP_DEFAULT_RECEIVE_TIMEOUT_MS, handle);
    }

    int32_t TimedExchange(uint16_t method, std::string_view url, std::string_view headers,
                          std::span<const uint8_t> body, uint32_t limit, uint32_t timeout, uint32_t& handle) override {
        return Begin(method, url, headers, body, limit, timeout, true, handle);
    }

    int32_t State(uint32_t handle, micropixel_http_state_t& state) override {
        auto* request = Find(handle);
        return request ? request->State(state) : MICROPIXEL_STATUS_STALE_STATE;
    }

    int32_t Metadata(uint32_t handle, micropixel_http_metadata_t& metadata) override {
        auto* request = Find(handle);
        return request ? request->Metadata(metadata) : MICROPIXEL_STATUS_STALE_STATE;
    }

    int32_t Read(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& length) override {
        auto* request = Find(handle);
        return request ? request->Read(false, offset, bytes, length) : MICROPIXEL_STATUS_STALE_STATE;
    }

    int32_t Headers(uint32_t handle, uint32_t offset, std::span<uint8_t> bytes, uint32_t& length) override {
        auto* request = Find(handle);
        return request ? request->Read(true, offset, bytes, length) : MICROPIXEL_STATUS_STALE_STATE;
    }

    int32_t Close(uint32_t handle) override {
        auto& lane = lanes_[handle & 1];
        if (!handle || handle != lane.handle) return MICROPIXEL_STATUS_STALE_STATE;
        lane.active->Close();
        lane.active = nullptr;
        lane.handle = 0;
        return MICROPIXEL_STATUS_OK;
    }

    void RunPending(uint32_t lane) {
        for (auto& request : lanes_[lane].requests) request.RunPending();
    }

   private:
    int32_t Begin(uint16_t method, std::string_view url, std::string_view headers, std::span<const uint8_t> body,
                  uint32_t limit, uint32_t timeout, bool exchange, uint32_t& handle) {
        if ((method != MICROPIXEL_HTTP_GET && method != MICROPIXEL_HTTP_POST) ||
            (method == MICROPIXEL_HTTP_GET && (!body.empty() || (!exchange && !headers.empty()))) || !limit ||
            limit > MICROPIXEL_HTTP_MAX_RESPONSE_BYTES || timeout < MICROPIXEL_HTTP_MIN_RECEIVE_TIMEOUT_MS ||
            timeout > MICROPIXEL_HTTP_MAX_RECEIVE_TIMEOUT_MS || url.size() <= 8 || url.size() >= kUrlCapacity ||
            url.find('\0') != url.npos || headers.size() > 4096 || body.size() > MICROPIXEL_HTTP_MAX_REQUEST_BYTES)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        auto& lane = lanes_[lane_];
        if (lane.handle) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        if (!mpx_wifi_online() || !mpx_network_time_ready()) return MICROPIXEL_STATUS_TIMEOUT;
        auto free = std::find_if(lane.requests.begin(), lane.requests.end(),
                                 [](const Request& request) { return request.GetPhase() != Request::Phase::kPending; });
        if (free == lane.requests.end()) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        const auto status = free->Begin(method, url, headers, body, limit, timeout, exchange);
        if (status) return status;
        if (++lane.generation > 0x7fffffffU) lane.generation = 1;
        lane.active = &*free;
        handle = lane.handle = (lane.generation << 1) | lane_;
        xTaskNotifyGive(workers[lane_].Handle());
        return MICROPIXEL_STATUS_OK;
    }

    Request* Find(uint32_t handle) {
        auto& lane = lanes_[handle & 1];
        return handle && handle == lane.handle ? lane.active : nullptr;
    }

    std::array<Lane, kLaneCount> lanes_{};
    uint32_t lane_{};
};

using HttpSessions = std::array<HttpSession, host::AppManager::kMaxInstances>;
HttpSessions* backends;

void Worker(void* raw) {
    const auto lane = reinterpret_cast<uintptr_t>(raw);
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (auto& session : *backends) session.RunPending(lane);
    }
}

}  // namespace

bool InitializeNetwork() {
    auto* memory = static_cast<HttpSessions*>(heap_caps_malloc(sizeof(HttpSessions), MALLOC_CAP_SPIRAM));
    if (!memory) return false;
    backends = std::construct_at(memory);
    if (mpx_https_initialize()) {
        for (uint32_t lane = 0; lane < workers.size(); ++lane) {
            if (!workers[lane].Start(Worker, "mpx_http", kHttpTaskStackBytes, reinterpret_cast<void*>(lane),
                                     task_policy::kRemoteControlPriority))
                break;
        }
        if (workers.back().Handle()) return true;
    }
    for (auto& task : workers) task.CancelStart();
    std::destroy_at(backends);
    heap_caps_free(memory);
    backends = nullptr;
    return false;
}

device::HttpClient* NetworkBackend(uint32_t slot) {
    return backends && slot < backends->size() ? &(*backends)[slot] : nullptr;
}

}  // namespace micropixel::bouffalo
