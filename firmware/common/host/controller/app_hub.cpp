// SPDX-License-Identifier: Apache-2.0
#include "host/controller/app_hub.hpp"

#include <algorithm>
#include <cstring>

#include "device/text.hpp"

namespace micropixel::host {
bool AppHub::ReadyService(uint32_t app, AppCaller& caller, device::AppEventSink& sink, bool running) const {
    if (app >= kMaxApps) return false;
    const auto& a = apps_[app];
    if (!a.accepting || !a.instance.valid() ||
        (a.components.service_state != 2 && (running || a.components.service_state != 1)) ||
        !a.components.service_token || !a.events.push)
        return false;
    caller = {app, a.instance, a.components.service_token};
    sink = a.events;
    return true;
}
int32_t AppHub::SendHome(uint32_t app, uint16_t type, uint32_t value, uint64_t sequence, uint64_t now,
                         uint32_t& ticket) {
    AppCaller target{};
    device::AppEventSink sink{};
    if (!ReadyService(app, target, sink)) return MICROPIXEL_STATUS_WOULD_BLOCK;
    micropixel_message_send_t r{};
    r.size = sizeof(r);
    r.type = type;
    r.sequence = sequence;
    r.ttl_ms = 5000;
    r.target_boot = target.instance.boot;
    r.target_slot = target.instance.slot;
    r.target_epoch = target.instance.epoch;
    r.target_component = target.component;
    r.payload_length = 4;
    std::memcpy(r.payload, &value, 4);
    return Send(Home(), r, now, ticket);
}
namespace {
bool Text(const char* text, size_t capacity, bool identifier = false) {
    const size_t length = strnlen(text, capacity);
    if (length == capacity || (identifier && (length == 0 || length > 64))) return false;
    if (length && !device::IsValidUtf8(reinterpret_cast<const uint8_t*>(text), length)) return false;
    for (size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127) return false;
        if (identifier && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                            c == '_' || c == '-'))
            return false;
    }
    return true;
}
template <class T>
bool Read(const uint8_t* bytes, uint32_t length, T& out) {
    if (!bytes || length != sizeof(T)) return false;
    std::memcpy(&out, bytes, sizeof(out));
    if (out.size != sizeof(T)) return false;
    if constexpr (requires { out.reserved0; })
        if (out.reserved0) return false;
    if constexpr (requires { out.reserved1; })
        if (out.reserved1) return false;
    return true;
}
template <class T>
int32_t Write(const T& value, uint8_t* bytes, uint32_t capacity, uint32_t& written) {
    written = sizeof(T);
    if (!bytes || capacity < sizeof(T)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(bytes, &value, sizeof(T));
    return MICROPIXEL_STATUS_OK;
}
bool Same(const AppCaller& a, const AppCaller& b) { return a.app == b.app && a.instance == b.instance; }
}  // namespace
bool AppHub::AllowWake(uint32_t app, std::string_view caller, uint32_t types) {
    if (app >= 64 || caller.empty() || caller.size() > 64 || caller.find('\0') != std::string_view::npos ||
        (types & 1U))
        return false;
    auto& rules = apps_[app].cold_rules;
    for (auto& rule : rules)
        if (!rule.caller[0] || caller == rule.caller) {
            std::memset(rule.caller, 0, sizeof(rule.caller));
            std::memcpy(rule.caller, caller.data(), caller.size());
            rule.types = types;
            return true;
        }
    return false;
}
int32_t AppHub::WakeTarget(const AppCaller& source, const micropixel_message_send_t& r) const {
    if (source.app >= 64 || !AliveCaller(source) || !apps_[source.app].accepting || !r.sequence ||
        r.sequence <= apps_[source.app].watermark || r.type < 1 || r.type > 31 ||
        r.flags != MICROPIXEL_MESSAGE_LOGICAL || r.target_component || r.target_boot || r.target_slot ||
        r.target_epoch || r.payload_length > 128 || r.ttl_ms < 100 || r.ttl_ms > 60000 ||
        !Text(r.target_app, sizeof(r.target_app), true))
        return -1;
    for (uint32_t app = 0; app < 64; ++app)
        if (std::strcmp(apps_[app].id, r.target_app) == 0) {
            for (const auto& rule : apps_[app].cold_rules)
                if (std::strcmp(rule.caller, apps_[source.app].id) == 0 && (rule.types & (1U << r.type))) return app;
        }
    return -1;
}
bool AppHub::Register(uint32_t index, std::string_view id, uint8_t widgets) {
    if (index >= apps_.size() || id.empty() || id.size() > 64 || widgets > 2) return false;
    for (const auto c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    for (uint32_t i = 0; i < apps_.size(); ++i)
        if (i != index && id == apps_[i].id) return false;
    auto& app = apps_[index];
    if (app.instance.valid()) return false;
    std::memset(app.id, 0, sizeof(app.id));
    std::memcpy(app.id, id.data(), id.size());
    app.widget_limit = widgets;
    return true;
}
void AppHub::Bind(uint32_t index, runtime::InstanceKey instance) {
    if (index >= apps_.size()) return;
    auto& app = apps_[index];
    app.instance = instance;
    app.components = {};
    app.events = {};
    app.rules = {};
    app.watermark = 0;
    app.accepting = true;
}
void AppHub::Remove(uint32_t app) {
    if (app >= 64) return;
    Quiesce({app, apps_[app].instance, 0});
    apps_[app] = {};
    for (auto& widget : widgets_)
        if (widget.used && widget.snapshot.app == app) widget = {};
}
bool AppHub::KeepClock(uint32_t app, uint64_t now) {
    if (app >= 64 || !apps_[app].widget_limit) return false;
    for (const auto& item : widgets_)
        if (item.used && item.snapshot.app == app && item.snapshot.data.widget_id == 1) return true;
    Published* selected = nullptr;
    for (auto& item : widgets_)
        if (!item.used) {
            selected = &item;
            break;
        }
    if (!selected) return false;
    selected->used = true;
    selected->pinned = true;
    auto& s = selected->snapshot;
    s = {};
    s.app = app;
    s.revision = ++next_revision_;
    s.published_ms = now;
    s.data.size = sizeof(s.data);
    s.data.template_id = MICROPIXEL_WIDGET_CLOCK;
    s.data.widget_id = 1;
    s.data.revision = 1;
    std::memcpy(s.data.title, "Clock", 6);
    return true;
}
bool AppHub::Valid(const AppCaller& caller) const {
    if (Same(caller, Home())) return caller.component == 0;
    return caller.app < apps_.size() && caller.instance.valid() && apps_[caller.app].instance == caller.instance;
}
bool AppHub::ComponentAlive(const AppCaller& caller) const {
    if (!Valid(caller)) return false;
    if (caller.app == kMaxApps) return true;
    const auto& info = apps_[caller.app].components;
    return caller.component && ((caller.component == info.activity_token && info.activity_state != 0) ||
                                (caller.component == info.service_token && info.service_state != 0));
}
void AppHub::SetSink(const AppCaller& caller, device::AppEventSink sink) {
    if (Valid(caller) && caller.app < kMaxApps) apps_[caller.app].events = sink;
}
void AppHub::Components(const AppCaller& caller, const micropixel_component_info_t& info) {
    if (!Valid(caller) || caller.app >= kMaxApps) return;
    auto& app = apps_[caller.app];
    if (app.components.service_token != info.service_token) app.rules = {};
    app.components = info;
    for (auto& message : messages_)
        if (message.ticket && !message.complete && Same(message.target, caller) && !ComponentAlive(message.target)) {
            message.complete = true;
            message.result = MICROPIXEL_STATUS_STALE_STATE;
        }
}
void AppHub::Quiesce(const AppCaller& caller) {
    if (!Valid(caller) || caller.app >= kMaxApps) return;
    apps_[caller.app].accepting = false;
    for (auto& message : messages_)
        if (message.ticket && !message.complete && (Same(message.target, caller) || Same(message.source, caller))) {
            message.complete = true;
            message.result = MICROPIXEL_STATUS_CANCELLED;
        }
}
void AppHub::Unbind(const AppCaller& caller) {
    if (!Valid(caller) || caller.app >= kMaxApps) return;
    Quiesce(caller);
    apps_[caller.app].instance = {};
    apps_[caller.app].events = {};
    apps_[caller.app].components = {};
    apps_[caller.app].rules = {};
}
int32_t AppHub::Publish(const AppCaller& caller, const micropixel_widget_snapshot_t& wire, uint64_t now,
                        uint32_t& revision) {
    if (!ComponentAlive(caller) || caller.app >= kMaxApps || (caller.component & 3) != MICROPIXEL_COMPONENT_SERVICE)
        return MICROPIXEL_STATUS_PERMISSION_DENIED;
    if (!wire.widget_id || wire.widget_id > apps_[caller.app].widget_limit || !wire.revision || wire.template_id < 1 ||
        wire.template_id > 3 || (wire.flags & ~(MICROPIXEL_WIDGET_PLAYING | MICROPIXEL_WIDGET_ERROR)) ||
        wire.valid_for_seconds > 86400 || wire.updated_utc > 4102444800ULL || wire.position_ms > 86400000 ||
        wire.duration_ms > 86400000 || !Text(wire.title, sizeof(wire.title)) ||
        !Text(wire.subtitle, sizeof(wire.subtitle)))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    Published* selected = nullptr;
    for (auto& item : widgets_)
        if (item.used && item.snapshot.app == caller.app && item.snapshot.data.widget_id == wire.widget_id) {
            selected = &item;
            break;
        }
    if (selected && selected->snapshot.instance == caller.instance &&
        selected->snapshot.component == caller.component && wire.revision <= selected->snapshot.data.revision)
        return MICROPIXEL_STATUS_STALE_STATE;
    if (!selected)
        for (auto& item : widgets_)
            if (!item.used) {
                selected = &item;
                break;
            }
    if (!selected)
        for (auto& item : widgets_)
            if (!item.pinned && !ComponentAlive({item.snapshot.app, item.snapshot.instance, item.snapshot.component}) &&
                (!selected || item.snapshot.published_ms < selected->snapshot.published_ms))
                selected = &item;
    if (!selected) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    const auto& previous = selected->snapshot;
    const bool changed = !selected->used || previous.instance != caller.instance ||
                         previous.component != caller.component || previous.data.template_id != wire.template_id ||
                         previous.data.flags != wire.flags || previous.data.value != wire.value ||
                         previous.data.auxiliary != wire.auxiliary || std::strcmp(previous.data.title, wire.title) ||
                         std::strcmp(previous.data.subtitle, wire.subtitle);
    uint32_t action_revision = previous.revision;
    if (changed) {
        if (++next_revision_ == 0) ++next_revision_;
        action_revision = next_revision_;
    }
    selected->used = true;
    selected->snapshot = {wire, caller.app, action_revision, caller.component, caller.instance, now, true, false};
    revision = action_revision;
    return MICROPIXEL_STATUS_OK;
}
AppHub::Message* AppHub::Find(uint32_t ticket) {
    for (auto& message : messages_)
        if (ticket && message.ticket == ticket) return &message;
    return nullptr;
}
const AppHub::Message* AppHub::Find(uint32_t ticket) const {
    for (const auto& message : messages_)
        if (ticket && message.ticket == ticket) return &message;
    return nullptr;
}
int32_t AppHub::Send(const AppCaller& source, const micropixel_message_send_t& request, uint64_t now,
                     uint32_t& ticket) {
    if (!Valid(source) || (source.component && !ComponentAlive(source))) return MICROPIXEL_STATUS_STALE_STATE;
    if (source.app < kMaxApps && !apps_[source.app].accepting) return MICROPIXEL_STATUS_CANCELLED;
    if (!request.sequence || !request.type || request.type > 31 || request.flags > MICROPIXEL_MESSAGE_LOGICAL ||
        request.payload_length > sizeof(request.payload) || request.ttl_ms < 100 || request.ttl_ms > 60000)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (request.flags == MICROPIXEL_MESSAGE_LOGICAL &&
        (!Text(request.target_app, sizeof(request.target_app), true) || request.target_boot || request.target_slot ||
         request.target_epoch))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (!request.flags && (!request.target_boot || !request.target_epoch)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    for (const auto& message : messages_)
        if (message.ticket && Same(message.source, source) && message.request.sequence == request.sequence) {
            if (message.source.component != source.component) return MICROPIXEL_STATUS_STALE_STATE;
            if (std::memcmp(&message.request, &request, sizeof(request))) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            ticket = message.ticket;
            return MICROPIXEL_STATUS_OK;
        }
    auto& watermark = source.app == kMaxApps ? home_watermark_ : apps_[source.app].watermark;
    if (request.sequence <= watermark) return MICROPIXEL_STATUS_STALE_STATE;
    uint32_t target_app = kMaxApps;
    for (uint32_t i = 0; i < apps_.size(); ++i) {
        if (request.flags == MICROPIXEL_MESSAGE_LOGICAL
                ? std::strcmp(apps_[i].id, request.target_app) == 0
                : apps_[i].instance ==
                      runtime::InstanceKey{request.target_boot, request.target_slot, request.target_epoch}) {
            target_app = i;
            break;
        }
    }
    if (target_app == kMaxApps || !apps_[target_app].instance.valid()) return MICROPIXEL_STATUS_NOT_FOUND;
    auto& target = apps_[target_app];
    if (!target.accepting) return MICROPIXEL_STATUS_CANCELLED;
    const uint32_t component = request.target_component ? request.target_component : target.components.service_token;
    const AppCaller destination{target_app, target.instance, component};
    if (!ComponentAlive(destination)) return MICROPIXEL_STATUS_STALE_STATE;
    const bool ready = (component == target.components.service_token && target.components.service_state == 2) ||
                       (component == target.components.activity_token && target.components.activity_state == 2);
    if (!ready) return MICROPIXEL_STATUS_WOULD_BLOCK;
    bool allowed = source.app == kMaxApps || source.app == target_app;
    if (!allowed)
        for (const auto& rule : target.rules)
            if (rule.component == component && std::strcmp(rule.caller, apps_[source.app].id) == 0 &&
                (rule.types & (1U << request.type)))
                allowed = true;
    if (!allowed)
        for (const auto& rule : target.cold_rules)
            if (std::strcmp(rule.caller, apps_[source.app].id) == 0 && (rule.types & (1U << request.type)))
                allowed = true;
    if (!allowed) return MICROPIXEL_STATUS_PERMISSION_DENIED;
    Message* selected = nullptr;
    for (auto& message : messages_)
        if (!message.ticket) {
            selected = &message;
            break;
        }
    if (!selected)
        for (auto& message : messages_)
            if (message.complete && (!selected || message.ticket < selected->ticket)) selected = &message;
    if (!selected) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    if (!target.events.push) return MICROPIXEL_STATUS_WOULD_BLOCK;
    if (++next_ticket_ == 0) ++next_ticket_;
    // Publish the record before exposing its immutable notification.
    *selected = {source, destination, request, now + request.ttl_ms, next_ticket_, 0, false};
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.service_id = MICROPIXEL_SERVICE_MESSAGE;
    event.event_id = MICROPIXEL_MESSAGE_EVENT;
    event.source = component;
    event.sequence = static_cast<uint32_t>(request.sequence);
    std::memcpy(event.payload, &next_ticket_, sizeof(next_ticket_));
    if (!target.events.push(target.events.context, event)) {
        *selected = {};
        return MICROPIXEL_STATUS_WOULD_BLOCK;
    }
    watermark = request.sequence;
    ticket = next_ticket_;
    return MICROPIXEL_STATUS_OK;
}
void AppHub::Process(uint64_t now) {
    for (auto& message : messages_)
        if (message.ticket && !message.complete && now >= message.deadline) {
            message.complete = true;
            message.result = MICROPIXEL_STATUS_TIMEOUT;
        }
}
bool AppHub::Pending(uint32_t app, runtime::InstanceKey key) const {
    for (const auto& message : messages_)
        if (message.ticket && !message.complete && message.target.app == app && message.target.instance == key)
            return true;
    return false;
}
bool AppHub::Widget(uint32_t index, uint64_t now, WidgetSnapshot& out) const {
    if (index >= widgets_.size() || !widgets_[index].used) return false;
    out = widgets_[index].snapshot;
    out.online = ComponentAlive({out.app, out.instance, out.component}) && apps_[out.app].accepting;
    out.expired = out.data.valid_for_seconds && now - out.published_ms > uint64_t(out.data.valid_for_seconds) * 1000;
    if (out.online && (out.data.flags & MICROPIXEL_WIDGET_PLAYING))
        out.data.position_ms =
            std::min<uint64_t>(out.data.duration_ms, uint64_t(out.data.position_ms) + now - out.published_ms);
    return true;
}
micropixel_message_result_t AppHub::Result(const AppCaller& caller, uint32_t ticket) const {
    const auto* message = Find(ticket);
    if (!Valid(caller) || !message || !Same(message->source, caller) || message->source.component != caller.component)
        return {sizeof(micropixel_message_result_t), 1, ticket, MICROPIXEL_STATUS_STALE_STATE};
    return {sizeof(micropixel_message_result_t), static_cast<uint16_t>(message->complete), ticket, message->result};
}
int32_t AppHub::Action(const WidgetSnapshot& expected, uint16_t type, uint32_t value, uint64_t sequence, uint64_t now,
                       uint32_t& ticket) {
    const auto app = expected.app, widget = expected.data.widget_id;
    const Published* item = nullptr;
    for (const auto& entry : widgets_)
        if (entry.used && entry.snapshot.app == app && entry.snapshot.data.widget_id == widget) {
            item = &entry;
            break;
        }
    if (!item || item->snapshot.revision != expected.revision || item->snapshot.instance != expected.instance ||
        item->snapshot.component != expected.component ||
        !ComponentAlive({app, item->snapshot.instance, item->snapshot.component}))
        return MICROPIXEL_STATUS_STALE_STATE;
    const auto kind = item->snapshot.data.template_id;
    if (!((kind == MICROPIXEL_WIDGET_MEDIA &&
           (type == MICROPIXEL_ACTION_SET_PLAYING || type == MICROPIXEL_ACTION_NEXT_TRACK)) ||
          (kind == MICROPIXEL_WIDGET_WEATHER && type == MICROPIXEL_ACTION_REFRESH) ||
          (kind == MICROPIXEL_WIDGET_CLOCK && type == MICROPIXEL_ACTION_OPEN_PAGE && value <= 2)) ||
        (type == MICROPIXEL_ACTION_SET_PLAYING && value > 1))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    micropixel_message_send_t request{};
    request.size = sizeof(request);
    request.type = type;
    request.sequence = sequence;
    request.target_boot = item->snapshot.instance.boot;
    request.target_slot = item->snapshot.instance.slot;
    request.target_epoch = item->snapshot.instance.epoch;
    request.target_component = item->snapshot.component;
    request.ttl_ms = 5000;
    request.payload_length = sizeof(value);
    std::memcpy(request.payload, &value, sizeof(value));
    return Send(Home(), request, now, ticket);
}
int32_t AppHub::Call(const AppCaller& caller, uint32_t service, uint32_t method, const uint8_t* request, uint32_t size,
                     uint8_t* out, uint32_t capacity, uint32_t& written, uint64_t now) {
    written = 0;
    if (!Valid(caller)) return MICROPIXEL_STATUS_STALE_STATE;
    Process(now);
    if (service == MICROPIXEL_SERVICE_WIDGET && method == MICROPIXEL_WIDGET_PUBLISH) {
        micropixel_widget_snapshot_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (!out || capacity < sizeof(micropixel_handle_response_t)) {
            written = sizeof(micropixel_handle_response_t);
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        uint32_t revision = 0;
        const auto result = Publish(caller, wire, now, revision);
        return result ? result
                      : Write(micropixel_handle_response_t{sizeof(micropixel_handle_response_t), 0, revision}, out,
                              capacity, written);
    }
    if (service != MICROPIXEL_SERVICE_MESSAGE) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (method == MICROPIXEL_MESSAGE_SEND) {
        micropixel_message_send_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (!out || capacity < sizeof(micropixel_handle_response_t)) {
            written = sizeof(micropixel_handle_response_t);
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        uint32_t ticket = 0;
        const auto status = Send(caller, wire, now, ticket);
        return status ? status
                      : Write(micropixel_handle_response_t{sizeof(micropixel_handle_response_t), 0, ticket}, out,
                              capacity, written);
    }
    if (method == MICROPIXEL_MESSAGE_ALLOW) {
        micropixel_message_allow_t wire{};
        if (!Read(request, size, wire) || !Text(wire.caller, sizeof(wire.caller), true))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (caller.app >= kMaxApps || !ComponentAlive(caller) ||
            caller.component != apps_[caller.app].components.service_token)
            return MICROPIXEL_STATUS_PERMISSION_DENIED;
        auto& rules = apps_[caller.app].rules;
        for (auto& rule : rules)
            if (!rule.component || std::strcmp(rule.caller, wire.caller) == 0) {
                std::memcpy(rule.caller, wire.caller, sizeof(rule.caller));
                rule.types = wire.types;
                rule.component = caller.component;
                return MICROPIXEL_STATUS_OK;
            }
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    if (method == MICROPIXEL_MESSAGE_ACK) {
        micropixel_message_ack_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        auto* message = Find(wire.ticket);
        if (!message || !Same(message->target, caller) || message->target.component != caller.component ||
            !ComponentAlive(caller))
            return MICROPIXEL_STATUS_STALE_STATE;
        if (message->complete)
            return (message->result == MICROPIXEL_STATUS_CANCELLED || message->result == MICROPIXEL_STATUS_TIMEOUT)
                       ? MICROPIXEL_STATUS_OK
                       : MICROPIXEL_STATUS_STALE_STATE;
        message->complete = true;
        message->result = wire.result;
        return MICROPIXEL_STATUS_OK;
    }
    if (method != MICROPIXEL_MESSAGE_RESULT && method != MICROPIXEL_MESSAGE_READ) return MICROPIXEL_STATUS_UNSUPPORTED;
    micropixel_handle_request_t wire{};
    if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (method == MICROPIXEL_MESSAGE_RESULT) return Write(Result(caller, wire.handle), out, capacity, written);
    if (method == MICROPIXEL_MESSAGE_READ) {
        const auto* message = Find(wire.handle);
        if (!message || !Same(message->target, caller) || caller.component != message->target.component ||
            !ComponentAlive(caller) || message->complete)
            return MICROPIXEL_STATUS_STALE_STATE;
        micropixel_message_received_t response{};
        response.size = sizeof(response);
        response.type = message->request.type;
        response.source_component = message->source.component;
        response.source_boot = message->source.instance.boot;
        response.source_slot = message->source.instance.slot;
        response.source_epoch = message->source.instance.epoch;
        response.sequence = message->request.sequence;
        response.payload_length = message->request.payload_length;
        std::memcpy(response.payload, message->request.payload, response.payload_length);
        const auto* id = message->source.app == kMaxApps ? "micropixel.home" : apps_[message->source.app].id;
        std::memcpy(response.source_app, id, std::strlen(id) + 1);
        return Write(response, out, capacity, written);
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}
}  // namespace micropixel::host
