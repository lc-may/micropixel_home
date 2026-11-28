// SPDX-License-Identifier: Apache-2.0
#include "runtime/abi/wireless_endpoint.hpp"

#include <cassert>
#include <cstring>

namespace {

mpx_wireless_status_t radio;
uint32_t command_method;
uint32_t command_revision;

}  // namespace

bool mpx_wireless_read(mpx_wireless_status_t* destination, uint32_t previous_revision) {
    if (radio.revision == previous_revision) return false;
    *destination = radio;
    return true;
}

bool mpx_wireless_read_wifi(uint32_t, uint32_t, mpx_wifi_network_t*, uint8_t*) { return false; }

int mpx_wireless_command(uint32_t method, uint32_t, uint32_t scan_revision, const char*, uint8_t) {
    command_method = method;
    command_revision = scan_revision;
    return 0;
}

int main() {
    micropixel::bouffalo::WirelessEndpoint endpoint;
    micropixel::runtime::ServiceRegistry services(endpoint);
    micropixel_service_info_t service{};
    assert(services.Open(MICROPIXEL_SERVICE_WIRELESS, MICROPIXEL_WIRELESS_MIN_VERSION, service, sizeof(service)) ==
           MICROPIXEL_STATUS_OK);
    assert(service.interface_major == 1 && service.interface_minor == 4);
    uint32_t size{};
    const auto call = [&](uint32_t method, const void* arg, uint32_t length, void* reply, uint32_t capacity) {
        return services.Call(service.service_handle, method, static_cast<const uint8_t*>(arg), length,
                             static_cast<uint8_t*>(reply), capacity, size);
    };
    assert(call(MICROPIXEL_WIRELESS_BLE_SCAN, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    endpoint.Authorize(true);

    radio.revision = 1;
    radio.ble_count = 1;
    radio.ble = MPX_BLE_IDLE;
    radio.ble_info.scan_revision = 7;
    radio.ble_info.flags = MPX_BLE_SCAN_COMPLETE;
    std::strcpy(radio.devices[0].scan.name, "Mouse");
    radio.devices[0].scan.appearance = 0x03c2;
    radio.devices[0].group = MPX_BT_DEVICE_INPUT;
    radio.devices[0].rssi = -56;

    micropixel_wireless_status_t status{};
    micropixel_wireless_ble_info_t details{};
    assert(call(MICROPIXEL_WIRELESS_STATUS, nullptr, 0, &status, sizeof(status)) == MICROPIXEL_STATUS_OK);
    assert(status.ble_count == 1);
    assert(call(MICROPIXEL_WIRELESS_BLE_INFO, nullptr, 0, &details, sizeof(details) - 1) ==
           MICROPIXEL_STATUS_BUFFER_TOO_SMALL);
    assert(size == sizeof(details));
    assert(call(MICROPIXEL_WIRELESS_BLE_INFO, nullptr, 0, &details, sizeof(details)) == MICROPIXEL_STATUS_OK);
    assert(details.scan_revision == 7 && details.flags == MPX_BLE_SCAN_COMPLETE);

    micropixel_wireless_ble_item_t item{};
    uint32_t legacy_index = 0;
    assert(call(MICROPIXEL_WIRELESS_BLE_ITEM, &legacy_index, sizeof(legacy_index), &item, sizeof(item)) ==
           MICROPIXEL_STATUS_OK);
    assert(std::strcmp(item.name, "Mouse") == 0 && item.rssi == -56);
    assert(call(MICROPIXEL_WIRELESS_BLE_CONNECT, &legacy_index, sizeof(legacy_index), nullptr, 0) ==
           MICROPIXEL_STATUS_OK);
    assert(command_method == MICROPIXEL_WIRELESS_BLE_CONNECT && command_revision == 7);

    const micropixel_wireless_ble_selection_t selected{.index = 0, .scan_revision = 7};
    micropixel_wireless_ble_item_details_t item_details{};
    assert(call(MICROPIXEL_WIRELESS_BLE_ITEM_DETAILS, &legacy_index, sizeof(legacy_index), &item_details,
                sizeof(item_details)) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(call(MICROPIXEL_WIRELESS_BLE_ITEM_DETAILS, &selected, sizeof(selected), &item_details,
                sizeof(item_details)) == MICROPIXEL_STATUS_OK);
    assert(size == sizeof(item_details) && item_details.appearance == 0x03c2 &&
           item_details.group == MPX_BT_DEVICE_INPUT && std::strcmp(item_details.item.name, "Mouse") == 0);
    ++radio.revision;
    ++radio.ble_info.scan_revision;
    assert(call(MICROPIXEL_WIRELESS_STATUS, nullptr, 0, &status, sizeof(status)) == MICROPIXEL_STATUS_OK);
    assert(call(MICROPIXEL_WIRELESS_BLE_ITEM, &selected, sizeof(selected), &item, sizeof(item)) ==
           MICROPIXEL_STATUS_NOT_FOUND);
    assert(call(MICROPIXEL_WIRELESS_BLE_ITEM_DETAILS, &selected, sizeof(selected), &item_details,
                sizeof(item_details)) == MICROPIXEL_STATUS_NOT_FOUND);
    command_method = 0;
    assert(call(MICROPIXEL_WIRELESS_BLE_CONNECT, &selected, sizeof(selected), nullptr, 0) ==
           MICROPIXEL_STATUS_NOT_FOUND);
    assert(command_method == 0);
    assert(call(MICROPIXEL_WIRELESS_BLE_STOP_SCAN, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_OK);
    assert(command_method == MICROPIXEL_WIRELESS_BLE_STOP_SCAN);

    ++radio.revision;
    radio.ble = MPX_BLE_INCOMING;
    radio.ble_info.flags = MPX_BLE_DISCOVERY_ENABLED;
    assert(call(MICROPIXEL_WIRELESS_STATUS, nullptr, 0, &status, sizeof(status)) == MICROPIXEL_STATUS_OK);
    assert(call(MICROPIXEL_WIRELESS_BLE_INFO, nullptr, 0, &details, sizeof(details)) == MICROPIXEL_STATUS_OK);
    assert(status.ble_state == MPX_BLE_INCOMING && (details.flags & MPX_BLE_DISCOVERY_ENABLED));
    assert(call(MICROPIXEL_WIRELESS_BLE_HIDE, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_OK);
    assert(command_method == MICROPIXEL_WIRELESS_BLE_HIDE);

    ++radio.revision;
    radio.ble_info.flags = 0;
    assert(call(MICROPIXEL_WIRELESS_STATUS, nullptr, 0, &status, sizeof(status)) == MICROPIXEL_STATUS_OK);
    assert(call(MICROPIXEL_WIRELESS_BLE_INFO, nullptr, 0, &details, sizeof(details)) == MICROPIXEL_STATUS_OK);
    assert(status.ble_state == MPX_BLE_INCOMING && !(details.flags & MPX_BLE_DISCOVERY_ENABLED));
}
