// SPDX-License-Identifier: Apache-2.0
#include "media_widgets.hpp"

#include <new>

#include "bflb_mtimer.h"
#include "freertos/task.h"
#include "mm.h"
#include "runtime/event_queue.hpp"

namespace micropixel::bouffalo {
namespace {

uint64_t NowMs() { return bflb_mtimer_get_time_us() / 1000U; }

// Orion runs on one core. All hub operations are bounded and the event sink
// only posts advisory events with a zero tick timeout.
struct HubLock final {
    HubLock() { taskENTER_CRITICAL(); }
    ~HubLock() { taskEXIT_CRITICAL(); }
};

}  // namespace

MediaWidgets* MediaWidgets::Create(uint32_t app, std::string_view id, uint64_t boot) {
    auto* storage = kmalloc(sizeof(MediaWidgets), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE);
    if (!storage) return nullptr;
    auto* widgets = new (storage) MediaWidgets(boot);
    if (widgets->hub_.Register(app, id, 1)) return widgets;
    widgets->~MediaWidgets();
    kfree(storage);
    return nullptr;
}

void MediaWidgets::Bind(const host::AppCaller& owner, runtime::EventQueue& events) {
    const HubLock lock;
    hub_.Bind(owner.app, owner.instance);
    hub_.SetSink(owner, {&events, [](void* raw, const micropixel_event_t& event) {
                             return static_cast<runtime::EventQueue*>(raw)->PushAdvisory(event);
                         }});
}

void MediaWidgets::SetComponents(const host::AppCaller& owner, const micropixel_component_info_t& info) {
    const HubLock lock;
    hub_.Components(owner, info);
}

void MediaWidgets::Quiesce(const host::AppCaller& owner) {
    const HubLock lock;
    hub_.Quiesce(owner);
}

void MediaWidgets::Unbind(const host::AppCaller& owner) {
    const HubLock lock;
    hub_.Unbind(owner);
}

bool MediaWidgets::Read(host::WidgetSnapshot& snapshot, bool& pending) {
    const HubLock lock;
    const auto now = NowMs();
    if (ticket_) {
        hub_.Process(now);
        if (hub_.Result(hub_.Home(), ticket_).complete) ticket_ = 0;
    }
    pending = ticket_ != 0;
    // Only one publisher and one widget are registered with this hub.
    return hub_.Widget(0, now, snapshot);
}

void MediaWidgets::Action(const host::WidgetSnapshot& expected, uint16_t type, uint32_t value) {
    const HubLock lock;
    if (!ticket_) (void)hub_.Action(expected, type, value, ++sequence_, NowMs(), ticket_);
}

int32_t MediaWidgets::Call(const host::AppCaller& caller, uint32_t service, uint32_t method, const uint8_t* request,
                           uint32_t size, uint8_t* response, uint32_t capacity, uint32_t& written) {
    const HubLock lock;
    return hub_.Call(caller, service, method, request, size, response, capacity, written, NowMs());
}

runtime::ServiceDescriptor MediaMessageEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_MESSAGE,
            .interface_major = 1,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
            .capabilities = session_.Available() ? 1U : 0U,
            .max_request_bytes = sizeof(micropixel_message_ack_t),
            .max_response_bytes = sizeof(micropixel_message_received_t)};
}

int32_t MediaMessageEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t size, uint8_t* response,
                                   uint32_t capacity, uint32_t& written) {
    if (method != MICROPIXEL_MESSAGE_READ && method != MICROPIXEL_MESSAGE_ACK) return MICROPIXEL_STATUS_UNSUPPORTED;
    return session_.Call(MICROPIXEL_SERVICE_MESSAGE, method, request, size, response, capacity, written);
}

}  // namespace micropixel::bouffalo
