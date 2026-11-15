// SPDX-License-Identifier: Apache-2.0
#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

namespace {
using namespace micropixel;
uint32_t handle{};
int32_t Call(uint32_t method, const void* request, uint32_t length, void* response = nullptr, uint32_t capacity = 0) {
    uint32_t written = 0;
    return micropixel_service_call(handle, method, static_cast<const uint8_t*>(request), length,
                                   static_cast<uint8_t*>(response), capacity, &written);
}
class ContractActivity final : public Activity {
    void OnCreate(ComponentContext& context) override {
        auto info = context.Status().value();
        Assert(info.activity_token == context.token(), "create token");
        micropixel_component_ack_t ack{sizeof(ack), 0, context.token(), UINT32_MAX, MICROPIXEL_COMPONENT_CREATE};
        Assert(Call(MICROPIXEL_LIFECYCLE_ACK, &ack, sizeof(ack)) == MICROPIXEL_STATUS_STALE_STATE,
               "incorrect ack accepted");
        micropixel_component_scope_t stale{sizeof(stale), 0, context.token() + 4};
        Assert(Call(MICROPIXEL_LIFECYCLE_SCOPE, &stale, sizeof(stale)) == MICROPIXEL_STATUS_STALE_STATE,
               "future generation accepted");
        // Direct service calls still require the active Activity resource domain.
        context.app().log().Info(
            "COMPONENT CONTRACT PASS wire sizes, reserved fields, admission, ack and component generations");
        context.Exit().value();
    }
};
}  // namespace
int main() {
    micropixel_service_info_t service{};
    Assert(micropixel_service_open(MICROPIXEL_SERVICE_LIFECYCLE, 65536, &service, sizeof(service)) == 0,
           "lifecycle service");
    handle = service.service_handle;
    micropixel_component_info_t info{};
    Assert(Call(MICROPIXEL_LIFECYCLE_QUERY, nullptr, 0, &info, sizeof(info)) == 0, "query");
    micropixel_component_register_request_t registration{sizeof(registration), info.flags, 0};
    Assert(Call(MICROPIXEL_LIFECYCLE_REGISTER, &registration, sizeof(registration), &info, sizeof(info) - 1) ==
               MICROPIXEL_STATUS_BUFFER_TOO_SMALL,
           "short registration output");
    Assert(Call(MICROPIXEL_LIFECYCLE_QUERY, nullptr, 0, &info, sizeof(info)) == 0 &&
               !(info.flags & MICROPIXEL_COMPONENT_FLAG_REGISTERED),
           "short call changed registration");
    registration.flags ^= MICROPIXEL_COMPONENT_FLAG_SERVICE;
    Assert(Call(MICROPIXEL_LIFECYCLE_REGISTER, &registration, sizeof(registration), &info, sizeof(info)) ==
               MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "undeclared component accepted");
    micropixel_component_scope_t scope{sizeof(scope), 1, 0};
    Assert(Call(MICROPIXEL_LIFECYCLE_SCOPE, &scope, sizeof(scope)) == MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "reserved bits accepted");
    scope.reserved0 = 0;
    scope.token = 999;
    Assert(Call(MICROPIXEL_LIFECYCLE_SCOPE, &scope, sizeof(scope)) == MICROPIXEL_STATUS_STALE_STATE,
           "foreign component token");
    Assert(Call(MICROPIXEL_LIFECYCLE_SCOPE, &scope, sizeof(scope) - 1) == MICROPIXEL_STATUS_INVALID_ARGUMENT,
           "truncated scope");
    micropixel_component_request_t request{sizeof(request), MICROPIXEL_COMPONENT_ACTIVITY, MICROPIXEL_COMPONENT_CREATE};
    Assert(Call(MICROPIXEL_LIFECYCLE_REQUEST, &request, sizeof(request)) == MICROPIXEL_STATUS_STALE_STATE,
           "request before registration");
    micropixel_component_route_t route{sizeof(route), 0, MICROPIXEL_SERVICE_TIMER, 99};
    Assert(
        Call(MICROPIXEL_LIFECYCLE_ROUTE, &route, sizeof(route), &scope, sizeof(scope)) == MICROPIXEL_STATUS_NOT_FOUND,
        "retired resource routed");
    Assert(Call(999, nullptr, 0) == MICROPIXEL_STATUS_UNSUPPORTED, "unknown lifecycle method");
    Application app;
    ContractActivity activity;
    app.RunComponents(&activity);
    return 0;
}
