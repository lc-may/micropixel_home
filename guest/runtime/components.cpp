// SPDX-License-Identifier: Apache-2.0
#include "sdk/components.hpp"

#include "runtime/service_binding.hpp"

namespace micropixel {
namespace {
runtime::ServiceCache lifecycle;
uint32_t current_scope{};
int32_t Call(uint32_t method, const void* request, uint32_t size, void* response, uint32_t capacity,
             uint32_t& written) {
    auto status = runtime::OpenService(lifecycle, MICROPIXEL_SERVICE_LIFECYCLE, 1, 0);
    if (!status && !lifecycle.info.capabilities) status = MICROPIXEL_STATUS_UNSUPPORTED;
    return status ? status : runtime::CallService(lifecycle, method, request, size, response, capacity, written);
}
void SetScope(uint32_t token) {
    if (current_scope == token) return;
    micropixel_component_scope_t wire{sizeof(wire), 0, token};
    uint32_t size = 0;
    runtime::RequireOk(Call(MICROPIXEL_LIFECYCLE_SCOPE, &wire, sizeof(wire), nullptr, 0, size), "component.scope");
    current_scope = token;
}
Result<void> Request(uint16_t kind, uint32_t command) {
    micropixel_component_request_t wire{sizeof(wire), kind, command};
    uint32_t written = 0;
    auto status = Call(MICROPIXEL_LIFECYCLE_REQUEST, &wire, sizeof(wire), nullptr, 0, written);
    if (status) return unexpected(runtime::ErrorFromStatus(status));
    return {};
}
Result<micropixel_component_info_t> Info() {
    micropixel_component_info_t wire{};
    uint32_t written = 0;
    auto status = Call(MICROPIXEL_LIFECYCLE_QUERY, nullptr, 0, &wire, sizeof(wire), written);
    if (status) return unexpected(runtime::ErrorFromStatus(status));
    if (written != sizeof(wire) || wire.size != sizeof(wire)) return unexpected(Error{ErrorCode::kInternal});
    return wire;
}
}  // namespace
namespace detail {
uint32_t EnterComponentScope(uint32_t token) {
    const auto previous = current_scope;
    SetScope(token);
    return previous;
}
void LeaveComponentScope(uint32_t previous) { SetScope(previous); }
}  // namespace detail
Result<void> ComponentContext::StartService() const {
    return Request(MICROPIXEL_COMPONENT_SERVICE, MICROPIXEL_COMPONENT_START);
}
Result<void> ComponentContext::StopService() const {
    return Request(MICROPIXEL_COMPONENT_SERVICE, MICROPIXEL_COMPONENT_STOP);
}
Result<void> ComponentContext::Exit() const { return Request(0, MICROPIXEL_COMPONENT_STOP); }
Result<void> ComponentContext::CreateActivity() const {
    return Request(MICROPIXEL_COMPONENT_ACTIVITY, MICROPIXEL_COMPONENT_CREATE);
}
Result<void> ComponentContext::DestroyActivity() const {
    return Request(MICROPIXEL_COMPONENT_ACTIVITY, MICROPIXEL_COMPONENT_DESTROY);
}
Result<ComponentStatus> ComponentContext::Status() const {
    auto info = Info();
    if (!info) return unexpected(info.error());
    return ComponentStatus{info->activity_token, info->activity_state, info->service_token, info->service_state,
                           info->resource_count};
}
void Application::RunComponents(Activity* activity, AppService* service, bool start_service) const {
    BeginRun();
    auto info = Info().value();
    const uint16_t flags = (activity ? MICROPIXEL_COMPONENT_FLAG_ACTIVITY : 0) |
                           (service ? MICROPIXEL_COMPONENT_FLAG_SERVICE : 0) |
                           (info.flags & MICROPIXEL_COMPONENT_FLAG_BACKGROUND);
    micropixel_component_register_request_t registration{sizeof(registration), flags,
                                                         service && start_service ? 1U : 0U};
    uint32_t written = 0;
    runtime::RequireOk(
        Call(MICROPIXEL_LIFECYCLE_REGISTER, &registration, sizeof(registration), &info, sizeof(info), written),
        "component.register");
    uint32_t activity_token = 0, service_token = 0;
    uint32_t activity_state = 0;
    bool service_running = false;
    for (;;) {
        auto event = WaitEvent();
        if (event.type() == EventType::kStop) {
            if (activity_token) {
                ComponentContext context(*this, activity_token);
                context.WithScope([&] {
                    if (activity_state == 2) activity->OnStop(context);
                    activity->OnDestroy(context);
                });
            }
            if (service_running) {
                ComponentContext context(*this, service_token);
                context.WithScope([&] { service->OnStop(context); });
            }
            break;
        }
        if (const auto* transition = event.lifecycle()) {
            ComponentContext context(*this, transition->token);
            context.WithScope([&] {
                if ((transition->token & 3) == MICROPIXEL_COMPONENT_ACTIVITY) {
                    if (!activity) runtime::Panic("component.activity.missing", MICROPIXEL_STATUS_INTERNAL);
                    switch (transition->command) {
                        case MICROPIXEL_COMPONENT_CREATE:
                            activity_token = transition->token;
                            activity_state = 1;
                            activity->OnCreate(context);
                            break;
                        case MICROPIXEL_COMPONENT_RESUME:
                            activity_state = 2;
                            activity->OnResume(context);
                            break;
                        case MICROPIXEL_COMPONENT_STOP:
                            activity_state = 3;
                            activity->OnStop(context);
                            break;
                        case MICROPIXEL_COMPONENT_DESTROY:
                            activity->OnDestroy(context);
                            activity_token = 0;
                            activity_state = 0;
                            break;
                        default:
                            runtime::Panic("component.activity.command", MICROPIXEL_STATUS_INTERNAL);
                    }
                } else {
                    if (!service) runtime::Panic("component.service.missing", MICROPIXEL_STATUS_INTERNAL);
                    if (transition->command == MICROPIXEL_COMPONENT_START) {
                        service_token = transition->token;
                        service_running = true;
                        service->OnStart(context);
                    } else if (transition->command == MICROPIXEL_COMPONENT_STOP) {
                        service->OnStop(context);
                        service_running = false;
                        service_token = 0;
                    } else
                        runtime::Panic("component.service.command", MICROPIXEL_STATUS_INTERNAL);
                }
            });
            micropixel_component_ack_t ack{sizeof(ack), 0, transition->token, transition->transition,
                                           transition->command};
            runtime::RequireOk(Call(MICROPIXEL_LIFECYCLE_ACK, &ack, sizeof(ack), nullptr, 0, written), "component.ack");
            continue;
        }
        micropixel_component_route_t route{sizeof(route), 0, event.origin_service(), event.origin_handle()};
        micropixel_component_scope_t target{};
        auto status = Call(MICROPIXEL_LIFECYCLE_ROUTE, &route, sizeof(route), &target, sizeof(target), written);
        if (status == MICROPIXEL_STATUS_NOT_FOUND || status == MICROPIXEL_STATUS_STALE_STATE) continue;
        runtime::RequireOk(status, "component.route");
        if (activity_token && activity_state != 3 && (!target.token || target.token == activity_token)) {
            ComponentContext context(*this, activity_token);
            context.WithScope([&] {
                if (event.type() == EventType::kNavigateBack) {
                    if (!activity->OnNavigateBack(context)) context.Exit().value();
                } else {
                    activity->OnEvent(context, event);
                }
            });
        }
        if (event.type() == EventType::kNavigateBack) continue;
        if (service_running && (!target.token || target.token == service_token)) {
            ComponentContext context(*this, service_token);
            context.WithScope([&] { service->OnEvent(context, event); });
        }
    }
    EndRun();
}
}  // namespace micropixel
