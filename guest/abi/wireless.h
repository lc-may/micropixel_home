// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "abi/micropixel_abi.h"

// Wireless 1.0 manages device-wide radio state. The Host grants access only
// to its trusted settings application.
#define MICROPIXEL_SERVICE_WIRELESS 29U
#define MICROPIXEL_WIRELESS_VERSION MICROPIXEL_INTERFACE_VERSION(1U, 0U)

enum {
    MICROPIXEL_WIRELESS_STATUS = 1,
    MICROPIXEL_WIRELESS_WIFI_ITEM,
    MICROPIXEL_WIRELESS_BLE_ITEM,
    MICROPIXEL_WIRELESS_WIFI_SCAN,
    MICROPIXEL_WIRELESS_WIFI_JOIN,
    MICROPIXEL_WIRELESS_WIFI_LEAVE,
    MICROPIXEL_WIRELESS_BLE_SCAN,
    MICROPIXEL_WIRELESS_BLE_ADVERTISE,
    MICROPIXEL_WIRELESS_BLE_HIDE,
    MICROPIXEL_WIRELESS_BLE_CONNECT,
    MICROPIXEL_WIRELESS_BLE_DISCONNECT,
};

enum {
    MPX_WIFI_STARTING,
    MPX_WIFI_IDLE,
    MPX_WIFI_JOINING,
    MPX_WIFI_DHCP,
    MPX_WIFI_ONLINE,
    MPX_WIFI_FAILED,
    MPX_WIFI_UNAVAILABLE
};

enum {
    MPX_BLE_STARTING,
    MPX_BLE_IDLE,
    MPX_BLE_SCANNING,
    MPX_BLE_ADVERTISING,
    MPX_BLE_CONNECTING,
    MPX_BLE_OUTGOING,
    MPX_BLE_INCOMING,
    MPX_BLE_UNAVAILABLE
};

// IP address is ordered as on the wire, one octet per byte. Errors are
// diagnostic values from the platform and are zero after acceptance.
typedef struct {
    uint32_t revision;
    uint8_t ipv4[4];
    int32_t wifi_error;
    int32_t ble_error;
    uint8_t wifi_state;
    uint8_t wifi_scanning;
    uint8_t wifi_count;
    uint8_t ble_state;
    uint8_t ble_count;
    uint8_t reserved[3];
} micropixel_wireless_status_t;

typedef struct {
    char ssid[33];
    uint8_t length;
    uint8_t open;
    uint8_t supported;
    int8_t rssi;
    uint8_t reserved[3];
} micropixel_wireless_wifi_item_t;

typedef struct {
    char name[33];
    char address[18];
    int8_t rssi;
} micropixel_wireless_ble_item_t;

typedef struct {
    uint32_t index;
    uint8_t length;
    uint8_t reserved[3];
    char password[64];
} micropixel_wireless_join_t;

#ifdef __cplusplus
static_assert(sizeof(micropixel_wireless_status_t) == 24);
static_assert(sizeof(micropixel_wireless_wifi_item_t) == 40);
static_assert(sizeof(micropixel_wireless_ble_item_t) == 52);
static_assert(sizeof(micropixel_wireless_join_t) == 72);
#endif
