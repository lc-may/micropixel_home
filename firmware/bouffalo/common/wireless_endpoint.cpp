// SPDX-License-Identifier: Apache-2.0
#include "wireless_endpoint.hpp"

#include <cstring>

namespace micropixel::bouffalo {
namespace {

template <typename T>
int32_t Reply(const T& value, uint8_t* response, uint32_t capacity, uint32_t& size) {
    size = sizeof(value);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(response, &value, size);
    return MICROPIXEL_STATUS_OK;
}

}  // namespace

runtime::ServiceDescriptor WirelessEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_WIRELESS,
            .interface_major = 1,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_request_bytes = sizeof(micropixel_wireless_join_t),
            .max_response_bytes = sizeof(micropixel_wireless_ble_item_t)};
}

int32_t WirelessEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                               uint32_t capacity, uint32_t& size) {
    if (!enabled_) return MICROPIXEL_STATUS_PERMISSION_DENIED;
    if (method == MICROPIXEL_WIRELESS_STATUS && length == 0) {
        mpx_wireless_read(&state_, state_.revision);
        micropixel_wireless_status_t value{.revision = state_.revision,
                                           .wifi_error = state_.wifi_error,
                                           .ble_error = state_.ble_error,
                                           .wifi_state = state_.wifi,
                                           .wifi_scanning = state_.wifi_scanning,
                                           .wifi_count = static_cast<uint8_t>(state_.wifi_count),
                                           .ble_state = state_.ble,
                                           .ble_count = static_cast<uint8_t>(state_.ble_count)};
        std::memcpy(value.ipv4, &state_.ipv4, sizeof(value.ipv4));
        return Reply(value, response, capacity, size);
    }

    uint32_t index = 0;
    if (method == MICROPIXEL_WIRELESS_WIFI_ITEM || method == MICROPIXEL_WIRELESS_BLE_ITEM ||
        method == MICROPIXEL_WIRELESS_BLE_CONNECT) {
        if (length != sizeof(index)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        std::memcpy(&index, request, sizeof(index));
        if (method == MICROPIXEL_WIRELESS_WIFI_ITEM) {
            if (index >= state_.wifi_count) return MICROPIXEL_STATUS_NOT_FOUND;
            const auto& network = state_.networks[index];
            micropixel_wireless_wifi_item_t item{
                .length = network.length, .open = network.open, .supported = network.supported, .rssi = network.rssi};
            std::memcpy(item.ssid, network.ssid, sizeof(item.ssid));
            return Reply(item, response, capacity, size);
        }
        if (method == MICROPIXEL_WIRELESS_BLE_ITEM) {
            if (index >= state_.ble_count) return MICROPIXEL_STATUS_NOT_FOUND;
            const auto& device = state_.devices[index];
            micropixel_wireless_ble_item_t item{.rssi = device.rssi};
            std::memcpy(item.name, device.name, sizeof(item.name));
            std::memcpy(item.address, device.address, sizeof(item.address));
            return Reply(item, response, capacity, size);
        }
    }

    const char* password = nullptr;
    uint8_t password_length = 0;
    if (method == MICROPIXEL_WIRELESS_WIFI_JOIN) {
        if (length != sizeof(micropixel_wireless_join_t)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        std::memcpy(&index, request, sizeof(index));
        password_length = request[offsetof(micropixel_wireless_join_t, length)];
        password = reinterpret_cast<const char*>(request + offsetof(micropixel_wireless_join_t, password));
        if (index >= state_.wifi_count || password_length > 64) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    } else if (method != MICROPIXEL_WIRELESS_BLE_CONNECT && length != 0) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }

    if (method < MICROPIXEL_WIRELESS_WIFI_SCAN || method > MICROPIXEL_WIRELESS_BLE_DISCONNECT) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    return mpx_wireless_command(method, index, password, password_length) == 0 ? MICROPIXEL_STATUS_OK
                                                                               : MICROPIXEL_STATUS_WOULD_BLOCK;
}

}  // namespace micropixel::bouffalo
