// SPDX-License-Identifier: Apache-2.0
#include "runtime/components.hpp"

#include <cstring>

#include "runtime/guest_context.hpp"

namespace micropixel::runtime {
namespace {
template <class T>
bool Read(const uint8_t* bytes, uint32_t size, T& out) {
    if (!bytes || size != sizeof(T)) return false;
    std::memcpy(&out, bytes, size);
    if (out.size != sizeof(T)) return false;
    if constexpr (requires { out.reserved0; })
        if (out.reserved0) return false;
    return true;
}
template <class T>
int32_t Write(const T& value, uint8_t* bytes, uint32_t capacity, uint32_t& size) {
    size = sizeof(T);
    if (!bytes || capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(bytes, &value, size);
    return MICROPIXEL_STATUS_OK;
}
}  // namespace
void Components::Wake() {
    micropixel_event_t wake{};
    wake.size = sizeof(wake);
    wake.service_id = MICROPIXEL_SERVICE_LIFECYCLE;
    (void)context_.events_.PushAdvisory(wake);
}
void Components::SetVisible(bool value, bool create) {
    portENTER_CRITICAL(&lock_);
    model_.visible = value;
    if (value && create && (model_.declared & MICROPIXEL_COMPONENT_FLAG_ACTIVITY)) model_.want_activity = true;
    portEXIT_CRITICAL(&lock_);
    Wake();
}
void Components::EnsureActivity() {
    portENTER_CRITICAL(&lock_);
    if (model_.declared & MICROPIXEL_COMPONENT_FLAG_ACTIVITY) model_.want_activity = true;
    portEXIT_CRITICAL(&lock_);
    Wake();
}
bool Components::TakeEvent(micropixel_event_t& event) {
    micropixel_component_event_payload_t payload{};
    portENTER_CRITICAL(&lock_);
    bool pending = model_.Next(payload);
    portEXIT_CRITICAL(&lock_);
    if (!pending) return false;
    event = {};
    event.size = sizeof(event);
    event.service_id = MICROPIXEL_SERVICE_LIFECYCLE;
    event.event_id = MICROPIXEL_LIFECYCLE_EVENT;
    event.source = payload.token;
    event.sequence = payload.transition;
    event.timestamp_us = context_.timers_.Now();
    std::memcpy(event.payload, &payload, sizeof(payload));
    return true;
}
micropixel_component_info_t Components::Info() const {
    micropixel_component_info_t info{};
    info.size = sizeof(info);
    portENTER_CRITICAL(&lock_);
    info.flags = model_.declared | (model_.registered ? MICROPIXEL_COMPONENT_FLAG_REGISTERED : 0);
    info.activity_token = model_.activity.token;
    info.activity_state = model_.activity.state;
    info.service_token = model_.service.token;
    info.service_state = model_.service.state;
    for (const auto& resource : resources_)
        if (resource.service_handle) ++info.resource_count;
    portEXIT_CRITICAL(&lock_);
    return info;
}
int32_t Components::Call(uint32_t method, const uint8_t* request, uint32_t size, uint8_t* output, uint32_t capacity,
                         uint32_t& written) {
    written = 0;
    if (!Available()) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (method == MICROPIXEL_LIFECYCLE_QUERY) {
        if (size) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return Write(Info(), output, capacity, written);
    }
    if (method == MICROPIXEL_LIFECYCLE_REGISTER) {
        micropixel_component_register_request_t wire{};
        if (!Read(request, size, wire) || wire.flags != model_.declared || wire.start_service > 1 ||
            (wire.start_service && !(wire.flags & MICROPIXEL_COMPONENT_FLAG_SERVICE)))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (!output || capacity < sizeof(micropixel_component_info_t)) {
            written = sizeof(micropixel_component_info_t);
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        portENTER_CRITICAL(&lock_);
        if (model_.registered) {
            portEXIT_CRITICAL(&lock_);
            return MICROPIXEL_STATUS_STALE_STATE;
        }
        model_.registered = true;
        model_.want_activity = wire.flags & MICROPIXEL_COMPONENT_FLAG_ACTIVITY;
        model_.want_service = wire.start_service;
        portEXIT_CRITICAL(&lock_);
        Wake();
        return Write(Info(), output, capacity, written);
    }
    if (method == MICROPIXEL_LIFECYCLE_SCOPE) {
        micropixel_component_scope_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        portENTER_CRITICAL(&lock_);
        if (wire.token && !model_.Alive(wire.token)) {
            portEXIT_CRITICAL(&lock_);
            return MICROPIXEL_STATUS_STALE_STATE;
        }
        model_.current_scope = wire.token;
        portEXIT_CRITICAL(&lock_);
        context_.devices_.audio().SetComponentScope(wire.token);
        return MICROPIXEL_STATUS_OK;
    }
    if (method == MICROPIXEL_LIFECYCLE_REQUEST) {
        micropixel_component_request_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        portENTER_CRITICAL(&lock_);
        bool valid = model_.registered;
        if (!valid) {
            portEXIT_CRITICAL(&lock_);
            return MICROPIXEL_STATUS_STALE_STATE;
        }
        if (wire.kind == 0 && wire.command == MICROPIXEL_COMPONENT_STOP) {
            portEXIT_CRITICAL(&lock_);
            return context_.RequestStop() ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
        }
        if (wire.kind == MICROPIXEL_COMPONENT_ACTIVITY && (model_.declared & MICROPIXEL_COMPONENT_FLAG_ACTIVITY) &&
            (wire.command == MICROPIXEL_COMPONENT_CREATE || wire.command == MICROPIXEL_COMPONENT_DESTROY)) {
            model_.want_activity = wire.command == MICROPIXEL_COMPONENT_CREATE;
        } else if (wire.kind == MICROPIXEL_COMPONENT_SERVICE && (model_.declared & MICROPIXEL_COMPONENT_FLAG_SERVICE) &&
                   (wire.command == MICROPIXEL_COMPONENT_START || wire.command == MICROPIXEL_COMPONENT_STOP)) {
            model_.want_service = wire.command == MICROPIXEL_COMPONENT_START;
        } else
            valid = false;
        portEXIT_CRITICAL(&lock_);
        if (valid) Wake();
        return valid ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (method == MICROPIXEL_LIFECYCLE_ACK) {
        micropixel_component_ack_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        portENTER_CRITICAL(&lock_);
        const auto* component = wire.token == model_.activity.token  ? &model_.activity
                                : wire.token == model_.service.token ? &model_.service
                                                                     : nullptr;
        bool valid = component && component->in_flight && component->transition == wire.transition &&
                     component->command == wire.command;
        portEXIT_CRITICAL(&lock_);
        if (!valid) return MICROPIXEL_STATUS_STALE_STATE;
        if (wire.command == MICROPIXEL_COMPONENT_DESTROY ||
            (wire.command == MICROPIXEL_COMPONENT_STOP && (wire.token & 3) == MICROPIXEL_COMPONENT_SERVICE))
            Cleanup(wire.token);
        portENTER_CRITICAL(&lock_);
        valid = model_.Ack(wire);
        portEXIT_CRITICAL(&lock_);
        Wake();
        return valid ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_STALE_STATE;
    }
    if (method == MICROPIXEL_LIFECYCLE_ROUTE) {
        micropixel_component_route_t wire{};
        if (!Read(request, size, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        uint32_t token = 0;
        bool found = wire.service_id == 0 || wire.service_id == MICROPIXEL_SERVICE_DEVICES;
        portENTER_CRITICAL(&lock_);
        if (wire.service_id == MICROPIXEL_SERVICE_INPUT) {
            if (model_.visible && model_.activity.state == 2) {
                token = model_.activity.token;
                found = true;
            }
        } else
            for (const auto& resource : resources_)
                if (resource.service_handle && resource.service == wire.service_id && resource.handle == wire.handle) {
                    token = resource.token;
                    found = true;
                    break;
                }
        const bool alive = !token || model_.Alive(token);
        portEXIT_CRITICAL(&lock_);
        if (!found) return MICROPIXEL_STATUS_NOT_FOUND;
        if (!alive) return MICROPIXEL_STATUS_STALE_STATE;
        return Write(micropixel_component_scope_t{sizeof(micropixel_component_scope_t), 0, token}, output, capacity,
                     written);
    }
    return MICROPIXEL_STATUS_UNSUPPORTED;
}
uint32_t Components::CloseMethod(uint32_t service, uint32_t method) {
    if (service == MICROPIXEL_SERVICE_TIMER && method == MICROPIXEL_TIMER_METHOD_CREATE)
        return MICROPIXEL_TIMER_METHOD_DESTROY;
    if (service == MICROPIXEL_SERVICE_RESOURCE) {
        if (method == MICROPIXEL_RESOURCE_METHOD_FONT_LOAD) return MICROPIXEL_RESOURCE_METHOD_FONT_UNLOAD;
        if (method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_LOAD ||
            method == MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE ||
            method == MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_UPDATE ||
            method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_DECODE)
            return MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD;
    }
    if (service == MICROPIXEL_SERVICE_AUDIO) {
        if (method == MICROPIXEL_AUDIO_METHOD_CLIP_LOAD) return MICROPIXEL_AUDIO_METHOD_CLIP_UNLOAD;
        if (method == MICROPIXEL_AUDIO_METHOD_PLAYBACK_START) return MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP;
        if (method == MICROPIXEL_AUDIO_METHOD_PCM_STREAM_OPEN) return MICROPIXEL_AUDIO_METHOD_PCM_STREAM_CLOSE;
        if (method == MICROPIXEL_AUDIO_METHOD_MEDIA_OPEN) return MICROPIXEL_AUDIO_METHOD_MEDIA_CLOSE;
    }
    if (service == MICROPIXEL_SERVICE_NETWORK &&
        (method == MICROPIXEL_NETWORK_METHOD_HTTP_START || method == MICROPIXEL_NETWORK_METHOD_HTTP_EXCHANGE ||
         method == MICROPIXEL_NETWORK_METHOD_HTTP_TIMED_EXCHANGE))
        return MICROPIXEL_NETWORK_METHOD_HTTP_CLOSE;
    if (service == MICROPIXEL_SERVICE_GRAPHICS && method == MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE)
        return MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY;
    return 0;
}
bool Components::CanDraw() const {
    portENTER_CRITICAL(&lock_);
    bool result = (model_.current_scope & 3) == MICROPIXEL_COMPONENT_ACTIVITY && model_.Alive(model_.current_scope);
    portEXIT_CRITICAL(&lock_);
    return result;
}
int32_t Components::BeforeCall(uint32_t service, uint32_t method) const {
    if (Available() && service == MICROPIXEL_SERVICE_GRAPHICS && method != MICROPIXEL_GRAPHICS_METHOD_GET_INFO &&
        method != MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT && !CanDraw())
        return MICROPIXEL_STATUS_PERMISSION_DENIED;
    portENTER_CRITICAL(&lock_);
    int32_t status = MICROPIXEL_STATUS_OK;
    if (model_.current_scope && !model_.Alive(model_.current_scope)) status = MICROPIXEL_STATUS_STALE_STATE;
    if (Available() && CloseMethod(service, method)) {
        bool free = false;
        for (const auto& resource : resources_) free |= !resource.service_handle;
        if (!free) status = MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    portEXIT_CRITICAL(&lock_);
    return status;
}
void Components::AfterCall(uint32_t service_handle, uint32_t service, uint32_t method, const uint8_t* request,
                           uint32_t request_size, const uint8_t* response, uint32_t response_size, int32_t status) {
    uint32_t handle = 0;
    const auto close = CloseMethod(service, method);
    if (close && status != MICROPIXEL_STATUS_OK) return;
    if (close && response && response_size >= 8)
        std::memcpy(&handle, response + 4, 4);
    else if (request && request_size >= 8)
        std::memcpy(&handle, request + 4, 4);
    if (!handle) return;
    portENTER_CRITICAL(&lock_);
    if (close && Available()) {
        for (auto& resource : resources_)
            if (!resource.service_handle) {
                resource = {service_handle, service, handle, model_.current_scope, close};
                break;
            }
    } else if (!close)
        for (auto& resource : resources_)
            if (resource.service == service && resource.handle == handle && resource.close_method == method)
                resource = {};
    portEXIT_CRITICAL(&lock_);
}
bool Components::StopAudioScoped() {
    portENTER_CRITICAL(&lock_);
    const auto token = model_.current_scope;
    portEXIT_CRITICAL(&lock_);
    if (!token) return false;
    context_.devices_.audio().StopComponent(token);
    for (size_t i = 0; i < resources_.size(); ++i) {
        portENTER_CRITICAL(&lock_);
        const auto resource = resources_[i];
        const bool close = resource.token == token && resource.service == MICROPIXEL_SERVICE_AUDIO &&
                           (resource.close_method == MICROPIXEL_AUDIO_METHOD_PLAYBACK_STOP ||
                            resource.close_method == MICROPIXEL_AUDIO_METHOD_PCM_STREAM_CLOSE ||
                            resource.close_method == MICROPIXEL_AUDIO_METHOD_MEDIA_CLOSE);
        if (close) resources_[i] = {};
        portEXIT_CRITICAL(&lock_);
        if (close) {
            micropixel_handle_request_t wire{sizeof(wire), 0, resource.handle};
            uint32_t n = 0;
            (void)context_.service_registry_.Call(resource.service_handle, resource.close_method,
                                                  reinterpret_cast<const uint8_t*>(&wire), sizeof(wire), nullptr, 0, n);
        }
    }
    return true;
}
void Components::Cleanup(uint32_t token) {
    context_.devices_.audio().StopComponent(token);
    if ((token & 3) == MICROPIXEL_COMPONENT_ACTIVITY) {
        context_.direct_surface_.Shutdown();
        context_.devices_.graphics().ReleaseActivityResources();
        (void)context_.devices_.input().EnableText(false);
    }
    for (size_t i = 0; i < resources_.size(); ++i) {
        portENTER_CRITICAL(&lock_);
        const auto resource = resources_[i];
        if (resource.token == token) resources_[i] = {};
        portEXIT_CRITICAL(&lock_);
        if (resource.token != token) continue;
        micropixel_handle_request_t request{sizeof(request), 0, resource.handle};
        uint32_t written = 0;
        (void)context_.service_registry_.Call(resource.service_handle, resource.close_method,
                                              reinterpret_cast<const uint8_t*>(&request), sizeof(request), nullptr, 0,
                                              written);
    }
}
}  // namespace micropixel::runtime
