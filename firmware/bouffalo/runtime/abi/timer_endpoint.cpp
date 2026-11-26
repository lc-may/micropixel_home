// SPDX-License-Identifier: Apache-2.0
#include "timer_endpoint.hpp"

#include <algorithm>
#include <cstring>

#include "bflb_mtimer.h"

namespace micropixel::bouffalo {
namespace {

template <typename Request>
bool ReadRequest(const uint8_t* request, uint32_t length, Request& wire) {
    if (request == nullptr || length != sizeof(wire)) return false;
    std::memcpy(&wire, request, length);
    return wire.size == length && wire.reserved0 == 0U;
}

}  // namespace

runtime::ServiceDescriptor TimerEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_TIMER,
            .interface_major = 1U,
            .interface_minor = 0U,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
            .max_request_bytes = sizeof(micropixel_timer_start_request_t),
            .max_response_bytes = sizeof(micropixel_handle_response_t)};
}

uint64_t TimerEndpoint::Now() const { return static_cast<uint64_t>(bflb_mtimer_get_time_us() - origin_us_); }

TimerEndpoint::Timer* TimerEndpoint::Find(uint32_t handle) {
    const uint32_t slot = handle & 0xffU;
    if (slot == 0U || slot > timers_.size() || timers_[slot - 1U].handle != handle) return nullptr;
    return &timers_[slot - 1U];
}

int32_t TimerEndpoint::Create(uint8_t* response, uint32_t capacity, uint32_t& response_size) {
    if (capacity < sizeof(micropixel_handle_response_t)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    for (uint32_t i = 0; i < timers_.size(); ++i) {
        auto& timer = timers_[i];
        if (timer.handle != 0U) continue;
        timer.generation = (timer.generation % 0x00ffffffU) + 1U;
        timer.handle = (timer.generation << 8U) | (i + 1U);
        const micropixel_handle_response_t reply{.size = sizeof(reply), .handle = timer.handle};
        std::memcpy(response, &reply, sizeof(reply));
        response_size = sizeof(reply);
        return MICROPIXEL_STATUS_OK;
    }
    return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
}

int32_t TimerEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                            uint32_t capacity, uint32_t& response_size) {
    if (method == MICROPIXEL_TIMER_METHOD_CREATE) {
        return length == 0U ? Create(response, capacity, response_size) : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method == MICROPIXEL_TIMER_METHOD_START) {
        micropixel_timer_start_request_t start{};
        if (!ReadRequest(request, length, start)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        Timer* timer = Find(start.timer_handle);
        const uint64_t now = Now();
        if (timer == nullptr || start.initial_delay_us == 0U || start.initial_delay_us >= UINT64_MAX - now) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        if (start.period_us != 0U && start.period_us != start.initial_delay_us) return MICROPIXEL_STATUS_UNSUPPORTED;
        timer->deadline_us = now + start.initial_delay_us;
        timer->last_us = now;
        timer->period_us = start.period_us;
        timer->sequence = 0U;
        return MICROPIXEL_STATUS_OK;
    }
    if (method != MICROPIXEL_TIMER_METHOD_CANCEL && method != MICROPIXEL_TIMER_METHOD_DESTROY) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    micropixel_handle_request_t ref{};
    if (!ReadRequest(request, length, ref)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    Timer* timer = Find(ref.handle);
    if (timer == nullptr) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    timer->deadline_us = 0U;
    if (method == MICROPIXEL_TIMER_METHOD_DESTROY) timer->handle = 0U;
    return MICROPIXEL_STATUS_OK;
}

void TimerEndpoint::Expire(Timer& timer, uint64_t now, micropixel_event_t& event) {
    uint64_t timestamp_us = timer.deadline_us;
    uint64_t missed = 0U;
    if (timer.period_us != 0U) {
        missed = (now - timer.deadline_us) / timer.period_us;
        timestamp_us = now - (now - timer.deadline_us) % timer.period_us;
    }
    timer.deadline_us =
        timer.period_us < UINT64_MAX - timestamp_us && timer.period_us != 0U ? timestamp_us + timer.period_us : 0U;
    const auto missed_count = static_cast<uint32_t>(std::min<uint64_t>(missed, UINT32_MAX));
    timer.sequence += missed_count + 1U;
    event = {};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_TIMER_EVENT_EXPIRED;
    event.service_id = MICROPIXEL_SERVICE_TIMER;
    event.timestamp_us = timestamp_us;
    event.source = timer.handle;
    event.sequence = timer.sequence;
    const micropixel_timer_event_payload_t payload{.elapsed_us = timestamp_us - timer.last_us,
                                                   .missed_count = missed_count};
    timer.last_us = timestamp_us;
    std::memcpy(event.payload, &payload, sizeof(payload));
}

runtime::EventWaitResult TimerEndpoint::Wait(runtime::EventQueue& queue, micropixel_event_t& event,
                                             uint64_t timeout_us) {
    using Result = runtime::EventWaitResult;
    Timer* next = nullptr;
    for (auto& timer : timers_) {
        if (timer.deadline_us != 0U && (next == nullptr || timer.deadline_us < next->deadline_us)) next = &timer;
    }
    if (next == nullptr) return queue.Wait(event, timeout_us);

    const uint64_t start = Now();
    const uint64_t delay = next->deadline_us <= start ? 0U : next->deadline_us - start;
    const auto result = queue.Wait(event, std::min(delay, timeout_us));
    if (result != Result::kTimeout || delay > timeout_us) return result;

    uint64_t now = Now();
    while (now < next->deadline_us) {
        // FreeRTOS timeouts are tick-relative and can end before an absolute
        // microsecond deadline. Wait until the absolute deadline.
        const auto retry = queue.Wait(event, next->deadline_us - now);
        if (retry != Result::kTimeout) return retry;
        now = Now();
    }
    Expire(*next, now, event);
    return Result::kReceived;
}

}  // namespace micropixel::bouffalo
