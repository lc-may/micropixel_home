// SPDX-License-Identifier: Apache-2.0
#include "runtime/abi/bluetooth_endpoint.hpp"

#include <cassert>

#include "platform/wireless/classic_bluetooth.h"

namespace {
constexpr uint32_t kSession = 27;
uint32_t remote_calls{};
uint32_t settings_calls{};
}  // namespace

namespace micropixel::bouffalo::classic {
micropixel_bluetooth_status_t Status() { return {.state = MPX_BT_CONNECTED, .role = MPX_BT_SINK}; }
int32_t Device(const micropixel_bluetooth_selection_t&, micropixel_bluetooth_device_t&) {
    return MICROPIXEL_STATUS_NOT_FOUND;
}
int32_t Bond(uint32_t, micropixel_bluetooth_address_t&) { return MICROPIXEL_STATUS_NOT_FOUND; }
int32_t Command(uint32_t, uint32_t, const micropixel_bluetooth_address_t*) {
    ++settings_calls;
    return MICROPIXEL_STATUS_OK;
}
int32_t ReadPcm(uint32_t, uint8_t*, uint32_t, uint32_t&) { return MICROPIXEL_STATUS_CLOSED; }
int32_t WritePcm(uint32_t, const uint8_t*, uint32_t) { return MICROPIXEL_STATUS_CLOSED; }
micropixel_bluetooth_remote_status_t RemoteStatus() {
    return {.session = kSession, .position_ms = 500, .duration_ms = 1200, .link = MPX_BT_REMOTE_READY};
}
int32_t RemoteKey(const micropixel_bluetooth_remote_key_t& request) {
    ++remote_calls;
    assert(request.key == MPX_BT_REMOTE_NEXT);
    return request.session == kSession ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_STALE_STATE;
}
}  // namespace micropixel::bouffalo::classic

int main() {
    micropixel::bouffalo::BluetoothEndpoint endpoint;
    micropixel::runtime::ServiceRegistry services(endpoint);
    micropixel_service_info_t service{};
    assert(services.Open(MICROPIXEL_SERVICE_BLUETOOTH, MICROPIXEL_BLUETOOTH_MIN_VERSION, service, sizeof(service)) ==
           MICROPIXEL_STATUS_OK);
    assert(service.interface_major == 1 && service.interface_minor == 2);
    uint32_t size{};
    const auto call = [&](uint32_t method, const void* input, uint32_t length, void* output, uint32_t capacity) {
        return services.Call(service.service_handle, method, static_cast<const uint8_t*>(input), length,
                             static_cast<uint8_t*>(output), capacity, size);
    };
    micropixel_bluetooth_status_t audio{};
    micropixel_bluetooth_remote_status_t remote{};
    micropixel_bluetooth_remote_key_t key{kSession, MPX_BT_REMOTE_NEXT};
    assert(call(MICROPIXEL_BT_STATUS, nullptr, 0, &audio, sizeof(audio)) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key), nullptr, 0) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    endpoint.Authorize(false, true);
    assert(call(MICROPIXEL_BT_STATUS, nullptr, 0, &audio, sizeof(audio)) == MICROPIXEL_STATUS_OK);
    assert(audio.role == MPX_BT_SINK);
    assert(call(MICROPIXEL_BT_REMOTE_STATUS, nullptr, 0, &remote, sizeof(remote) - 1) ==
           MICROPIXEL_STATUS_BUFFER_TOO_SMALL);
    assert(size == sizeof(remote));
    assert(call(MICROPIXEL_BT_REMOTE_STATUS, nullptr, 0, &remote, sizeof(remote)) == MICROPIXEL_STATUS_OK);
    assert(remote.session == kSession && remote.link == MPX_BT_REMOTE_READY);
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key) - 1, nullptr, 0) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(remote_calls == 0);
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key), nullptr, 0) == MICROPIXEL_STATUS_OK);
    --key.session;
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key), nullptr, 0) == MICROPIXEL_STATUS_STALE_STATE);
    assert(remote_calls == 2);
    assert(call(MICROPIXEL_BT_DISCONNECT, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    assert(settings_calls == 0);
    endpoint.Authorize(true, false);
    assert(call(MICROPIXEL_BT_DISCONNECT, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_OK);
    assert(settings_calls == 1);
}
