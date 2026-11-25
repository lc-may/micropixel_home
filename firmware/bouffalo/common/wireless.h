// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/wireless.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { MPX_WIFI_RESULTS = 14, MPX_BLE_RESULTS = 16 };

typedef struct {
    char ssid[33];
    uint8_t length;
    bool open;
    bool supported;
    int8_t rssi;
} mpx_wifi_network_t;

typedef struct {
    char name[33];
    char address[18];
    uint8_t mac[6];
    uint8_t address_type;
    int8_t rssi;
} mpx_ble_device_t;

typedef struct {
    uint32_t revision;
    uint8_t wifi;
    bool wifi_scanning;
    int wifi_error;
    uint32_t ipv4;
    unsigned wifi_count;
    mpx_wifi_network_t networks[MPX_WIFI_RESULTS];
    uint8_t ble;
    int ble_error;
    unsigned ble_count;
    mpx_ble_device_t devices[MPX_BLE_RESULTS];
} mpx_wireless_status_t;

// Initialize before main returns. Poll and commands run on the Host controller.
void mpx_wireless_init(void);
void mpx_wireless_poll(uint32_t now_ms);
bool mpx_wireless_read(mpx_wireless_status_t* destination, uint32_t previous_revision);
bool mpx_wifi_online(void);
int mpx_wireless_command(uint32_t method, uint32_t index, const char* password, uint8_t length);
#ifdef __cplusplus
}
#endif
