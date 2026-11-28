// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/wireless.h"
#include "bluetooth_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { MPX_WIFI_RESULTS = 100, MPX_BLE_RESULTS = 16, MPX_WIFI_NO_INDEX = UINT8_MAX };

typedef struct {
    char ssid[33];
    uint8_t length;
    bool open;
    bool supported;
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
    uint8_t ssid_bands;
    uint8_t preferred;
    uint8_t fallback;
} mpx_wifi_network_t;

typedef struct {
    mpx_bt_scan_data_t scan;
    char address[18];
    uint8_t mac[6];
    uint8_t address_type;
    int8_t rssi;
    uint8_t group;
} mpx_ble_device_t;

typedef struct {
    uint32_t revision;
    uint8_t wifi;
    bool wifi_scanning;
    int wifi_error;
    uint32_t ipv4;
    unsigned wifi_count;
    uint32_t wifi_scan_revision;
    uint8_t ble;
    int ble_error;
    unsigned ble_count;
    micropixel_wireless_ble_info_t ble_info;
    mpx_ble_device_t devices[MPX_BLE_RESULTS];
} mpx_wireless_status_t;

// Initialize before main returns. Poll and commands run on the Host controller.
void mpx_wireless_init(void);
void mpx_wireless_poll(uint32_t now_ms);
bool mpx_wireless_read(mpx_wireless_status_t* destination, uint32_t previous_revision);
bool mpx_wireless_read_wifi(uint32_t revision, uint32_t index, mpx_wifi_network_t* network, uint8_t* preferred_channel);
bool mpx_wifi_online(void);
int mpx_wireless_command(uint32_t method, uint32_t index, uint32_t scan_revision, const char* password, uint8_t length);
#ifdef __cplusplus
}
#endif
