// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#include "host/controller/app_hub.hpp"

namespace {
namespace host = micropixel::host;
struct Inbox {
    unsigned count{};
    bool accepts{true};
    micropixel_event_t last{};
    static bool Push(void* raw, const micropixel_event_t& event) {
        auto& self = *static_cast<Inbox*>(raw);
        if (!self.accepts) return false;
        ++self.count;
        self.last = event;
        return true;
    }
};
template <class Request, class Response>
int32_t Call(host::AppHub& hub, const host::AppCaller& caller, uint32_t service, uint32_t method,
             const Request& request, Response& response, uint64_t now = 100) {
    uint32_t written = 0;
    return hub.Call(caller, service, method, reinterpret_cast<const uint8_t*>(&request), sizeof(request),
                    reinterpret_cast<uint8_t*>(&response), sizeof(response), written, now);
}
template <class Request>
int32_t Control(host::AppHub& hub, const host::AppCaller& caller, uint32_t method, const Request& request,
                uint64_t now = 100) {
    uint32_t n = 0;
    return hub.Call(caller, MICROPIXEL_SERVICE_MESSAGE, method, reinterpret_cast<const uint8_t*>(&request),
                    sizeof(request), nullptr, 0, n, now);
}
micropixel_component_info_t Ready() { return {sizeof(micropixel_component_info_t), 7, 5, 2, 6, 2, 0}; }
micropixel_widget_snapshot_t Snapshot(uint32_t id, uint32_t revision, uint16_t kind = MICROPIXEL_WIDGET_MEDIA) {
    micropixel_widget_snapshot_t value{};
    value.size = sizeof(value);
    value.template_id = kind;
    value.widget_id = id;
    value.revision = revision;
    value.valid_for_seconds = 10;
    value.flags = MICROPIXEL_WIDGET_PLAYING;
    value.position_ms = 1000;
    value.duration_ms = 10000;
    std::strcpy(value.title, "Fixture");
    return value;
}
void TestHub() {
    auto owned = std::make_unique<host::AppHub>(42);
    auto& hub = *owned;
    Inbox a_inbox, b_inbox;
    const host::AppCaller a{0, {42, 0, 1}, 6}, b{1, {42, 1, 1}, 6};
    assert(hub.Register(0, "app.a", 2) && hub.Register(1, "app.b", 2));
    hub.Bind(0, a.instance);
    hub.Bind(1, b.instance);
    hub.Components(a, Ready());
    hub.Components(b, Ready());
    hub.SetSink(a, {&a_inbox, Inbox::Push});
    hub.SetSink(b, {&b_inbox, Inbox::Push});
    auto snapshot = Snapshot(1, 1);
    micropixel_handle_response_t handle{};
    uint32_t written = 0;
    assert(hub.Call(a, MICROPIXEL_SERVICE_WIDGET, MICROPIXEL_WIDGET_PUBLISH,
                    reinterpret_cast<const uint8_t*>(&snapshot), sizeof(snapshot), reinterpret_cast<uint8_t*>(&handle),
                    sizeof(handle) - 1, written, 100) == MICROPIXEL_STATUS_BUFFER_TOO_SMALL);
    host::WidgetSnapshot view{};
    assert(!hub.Widget(0, 100, view));
    assert(Call(hub, a, MICROPIXEL_SERVICE_WIDGET, MICROPIXEL_WIDGET_PUBLISH, snapshot, handle) == 0);
    assert(hub.Widget(0, 1100, view) && view.online && view.data.position_ms == 2000);
    auto old = view;
    snapshot.revision = 2;
    snapshot.value = 1;  // New track / action state.
    assert(Call(hub, a, MICROPIXEL_SERVICE_WIDGET, MICROPIXEL_WIDGET_PUBLISH, snapshot, handle) == 0);
    assert(Call(hub, a, MICROPIXEL_SERVICE_WIDGET, MICROPIXEL_WIDGET_PUBLISH, snapshot, handle) ==
           MICROPIXEL_STATUS_STALE_STATE);
    uint32_t ticket = 0;
    assert(hub.Action(old, MICROPIXEL_ACTION_NEXT_TRACK, 0, 1, 100, ticket) == MICROPIXEL_STATUS_STALE_STATE);
    assert(hub.Widget(0, 100, view));
    assert(hub.Action(view, MICROPIXEL_ACTION_SET_PLAYING, 0, 1, 100, ticket) == 0 && a_inbox.count == 1);
    micropixel_handle_request_t read{sizeof(read), 0, ticket};
    micropixel_message_received_t received{};
    assert(Call(hub, b, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_READ, read, received) ==
           MICROPIXEL_STATUS_STALE_STATE);
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_READ, read, received) == 0);
    assert(std::strcmp(received.source_app, "micropixel.home") == 0 && received.source_boot == 42 &&
           received.type == MICROPIXEL_ACTION_SET_PLAYING);
    micropixel_message_ack_t ack{sizeof(ack), 0, ticket, 0};
    assert(Control(hub, a, MICROPIXEL_MESSAGE_ACK, ack) == 0);
    assert(hub.Result(hub.Home(), ticket).complete && hub.Result(hub.Home(), ticket).result == 0);
    assert(Control(hub, a, MICROPIXEL_MESSAGE_ACK, ack) == MICROPIXEL_STATUS_STALE_STATE);

    micropixel_message_send_t send{};
    send.size = sizeof(send);
    send.type = MICROPIXEL_ACTION_NEXT_TRACK;
    send.sequence = 1;
    send.flags = MICROPIXEL_MESSAGE_LOGICAL;
    send.ttl_ms = 1000;
    std::strcpy(send.target_app, "app.b");
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) ==
           MICROPIXEL_STATUS_PERMISSION_DENIED);
    micropixel_message_allow_t rule{};
    rule.size = sizeof(rule);
    rule.types = 1U << MICROPIXEL_ACTION_NEXT_TRACK;
    std::strcpy(rule.caller, "app.a");
    assert(Control(hub, b, MICROPIXEL_MESSAGE_ALLOW, rule) == 0);
    b_inbox.accepts = false;
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) ==
           MICROPIXEL_STATUS_WOULD_BLOCK);
    b_inbox.accepts = true;
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) == 0);
    const auto first = handle.handle;
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) == 0 &&
           handle.handle == first && b_inbox.count == 1);
    send.payload_length = 1;
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) ==
           MICROPIXEL_STATUS_INVALID_ARGUMENT);
    send.payload_length = 0;
    ack.ticket = first;
    assert(Control(hub, b, MICROPIXEL_MESSAGE_ACK, ack) == 0);
    assert(hub.Result(a, first).result == 0);
    auto wrong_component = a;
    wrong_component.component = 5;
    assert(hub.Result(wrong_component, first).result == MICROPIXEL_STATUS_STALE_STATE);
    // Result-cache eviction preserves the sender watermark: old Next is rejected.
    for (uint64_t sequence = 2; sequence < 42; ++sequence) {
        assert(hub.Action(view, MICROPIXEL_ACTION_NEXT_TRACK, 0, sequence, 100, ticket) == 0);
        ack.ticket = ticket;
        assert(Control(hub, a, MICROPIXEL_MESSAGE_ACK, ack) == 0);
    }
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) ==
               MICROPIXEL_STATUS_STALE_STATE &&
           b_inbox.count == 1);
    for (uint64_t sequence = 100; sequence < 132; ++sequence)
        assert(hub.Action(view, MICROPIXEL_ACTION_NEXT_TRACK, 0, sequence, 100, ticket) == 0);
    assert(hub.Action(view, MICROPIXEL_ACTION_NEXT_TRACK, 0, 132, 100, ticket) == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    assert(hub.Pending(0, a.instance));
    hub.Process(10000);
    assert(!hub.Pending(0, a.instance));
    assert(hub.Result(hub.Home(), ticket).result == MICROPIXEL_STATUS_TIMEOUT);
    auto retired = b.instance;
    hub.Unbind(b);
    hub.Bind(1, {42, 1, 2});
    auto replacement = b;
    replacement.instance.epoch = 2;
    hub.Components(replacement, Ready());
    hub.SetSink(replacement, {&b_inbox, Inbox::Push});
    send.sequence = 2;
    send.flags = 0;
    send.target_boot = retired.boot;
    send.target_slot = retired.slot;
    send.target_epoch = retired.epoch;
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) ==
           MICROPIXEL_STATUS_NOT_FOUND);
    send.target_epoch = 2;
    assert(Call(hub, a, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, handle) ==
           MICROPIXEL_STATUS_PERMISSION_DENIED);
    auto info = Ready();
    info.service_token = 0;
    info.service_state = 0;
    hub.Components(a, info);
    assert(hub.Widget(0, 20000, view) && !view.online && view.expired);
    assert(hub.Action(view, MICROPIXEL_ACTION_NEXT_TRACK, 0, 133, 20000, ticket) == MICROPIXEL_STATUS_STALE_STATE);
}
void TestColdPolicy() {
    auto owned = std::make_unique<host::AppHub>(123);
    auto& hub = *owned;
    assert(hub.Register(0, "sender", 0) && hub.Register(1, "receiver", 0));
    host::AppCaller sender{0, {123, 0, 1}, 6};
    hub.Bind(0, sender.instance);
    hub.Components(sender, Ready());
    micropixel_message_send_t send{};
    send.size = sizeof(send);
    send.type = 3;
    send.sequence = 1;
    send.flags = 1;
    send.ttl_ms = 1000;
    std::strcpy(send.target_app, "receiver");
    assert(hub.WakeTarget(sender, send) < 0);
    assert(hub.AllowWake(1, "sender", 1U << 3));
    assert(hub.WakeTarget(sender, send) == 1);
    auto invalid = send;
    invalid.flags = 0;
    invalid.target_boot = 123;
    invalid.target_epoch = 1;
    assert(hub.WakeTarget(sender, invalid) < 0);
    invalid = send;
    invalid.type = 2;
    assert(hub.WakeTarget(sender, invalid) < 0);
    host::AppCaller receiver{1, {123, 1, 1}, 6};
    Inbox inbox;
    hub.Bind(1, receiver.instance);
    hub.Components(receiver, Ready());
    hub.SetSink(receiver, {&inbox, Inbox::Push});
    micropixel_handle_response_t result{};
    assert(Call(hub, sender, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, result) == 0 &&
           inbox.count == 1);
    hub.Unbind(receiver);
    receiver.instance.epoch++;
    hub.Bind(1, receiver.instance);
    hub.Components(receiver, Ready());
    hub.SetSink(receiver, {&inbox, Inbox::Push});
    assert(hub.Result(sender, result.handle).result == MICROPIXEL_STATUS_CANCELLED);
    send.sequence = 2;
    assert(Call(hub, sender, MICROPIXEL_SERVICE_MESSAGE, MICROPIXEL_MESSAGE_SEND, send, result) == 0);
    hub.Quiesce(sender);
    send.sequence = 3;
    assert(hub.WakeTarget(sender, send) < 0);
}
void TestQuota() {
    auto owned = std::make_unique<host::AppHub>(43);
    auto& hub = *owned;
    micropixel_handle_response_t response{};
    for (uint32_t app = 0; app < 5; ++app) {
        const auto id = std::string("quota.") + std::to_string(app);
        assert(hub.Register(app, id, 2));
        host::AppCaller caller{app, {43, app, 1}, 6};
        hub.Bind(app, caller.instance);
        hub.Components(caller, Ready());
        for (unsigned widget = 1; widget <= 2; ++widget) {
            auto value = Snapshot(widget, 1);
            const auto result =
                Call(hub, caller, MICROPIXEL_SERVICE_WIDGET, MICROPIXEL_WIDGET_PUBLISH, value, response);
            assert(result == (app < 4 ? 0 : MICROPIXEL_STATUS_RESOURCE_EXHAUSTED));
        }
    }
}
}  // namespace
int main() {
    TestHub();
    TestQuota();
    TestColdPolicy();
    std::puts(
        "PASS Widget quotas/revisions and Broker identity, allowlist, dedup, result eviction, deadlines and stale "
        "instances");
}
