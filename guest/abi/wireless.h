// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "abi/bluetooth_scan.h"
#include "abi/micropixel_abi.h"

// Wireless manages device-wide radio state. The Host grants access only
// to its trusted settings application.
#define MICROPIXEL_SERVICE_WIRELESS 29U
#define MICROPIXEL_WIRELESS_MIN_VERSION MICROPIXEL_INTERFACE_VERSION(1U, 0U)
#define MICROPIXEL_WIRELESS_VERSION MICROPIXEL_INTERFACE_VERSION(1U, 4U)

enum {
    MICROPIXEL_WIRELESS_STATUS = 1,
    MICROPIXEL_WIRELESS_WIFI_ITEM,
    MICROPIXEL_WIRELESS_BLE_ITEM,
    MICROPIXEL_WIRELESS_WIFI_SCAN,
    MICROPIXEL_WIRELESS_WIFI_JOIN,
    MICROPIXEL_WIRELESS_WIFI_LEAVE,
    MICROPIXEL_WIRELESS_BLE_SCAN,
    MICROPIXEL_WIRELESS_BLE_ADVERTISE,
    MICROPIXEL_WIRELESS_BLE_HIDE,  // In 1.4, also disables discovery during an incoming link.
    MICROPIXEL_WIRELESS_BLE_CONNECT,
    MICROPIXEL_WIRELESS_BLE_DISCONNECT,  // In 1.2, also cancels an outgoing connection.
    MICROPIXEL_WIRELESS_BLE_INFO,
    MICROPIXEL_WIRELESS_BLE_STOP_SCAN,
    MICROPIXEL_WIRELESS_BLE_ITEM_DETAILS,
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

enum { MPX_WIFI_BAND_2_4_GHZ = 1U << 0, MPX_WIFI_BAND_5_GHZ = 1U << 1 };

enum {
    MPX_BLE_SCAN_COMPLETE = 1U << 0,
    MPX_BLE_COMMAND_PENDING = 1U << 1,
    MPX_BLE_DISCONNECT_PENDING = 1U << 2,
    MPX_BLE_DISCOVERY_ENABLED = 1U << 3,  // Wireless 1.4. Remains set during an incoming link.
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
    // Added in Wireless 1.1. These bytes are zero on a 1.0 Host.
    uint8_t channel;
    uint8_t ssid_bands;         // MPX_WIFI_BAND_* mask for this SSID in the scan.
    uint8_t preferred_channel;  // Channel the Host will try first when joining this item.
} micropixel_wireless_wifi_item_t;

typedef struct {
    char name[33];
    char address[18];
    int8_t rssi;
} micropixel_wireless_ble_item_t;

// Wireless 1.3. BLE_ITEM_DETAILS uses micropixel_wireless_ble_selection_t.
typedef struct {
    micropixel_wireless_ble_item_t item;
    uint16_t appearance;
    uint8_t group;  // MPX_BT_DEVICE_*
    uint8_t reserved;
} micropixel_wireless_ble_item_details_t;

// Wireless 1.2. BLE_INFO describes the snapshot returned by the last STATUS call.
typedef struct {
    uint32_t scan_revision;
    uint8_t flags;
    char local_name[33];
    char peer_name[33];
    char peer_address[18];
    uint8_t reserved[3];
} micropixel_wireless_ble_info_t;

// Wireless 1.2 also accepts this request for BLE_ITEM and BLE_CONNECT.
// The index must belong to scan_revision. The uint32_t index form remains valid.
typedef struct {
    uint32_t index;
    uint32_t scan_revision;
} micropixel_wireless_ble_selection_t;

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
static_assert(sizeof(micropixel_wireless_ble_item_details_t) == 56);
static_assert(sizeof(micropixel_wireless_ble_info_t) == 92);
static_assert(sizeof(micropixel_wireless_ble_selection_t) == 8);
static_assert(sizeof(micropixel_wireless_join_t) == 72);
#endif
