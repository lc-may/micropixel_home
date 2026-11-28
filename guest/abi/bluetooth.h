// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "abi/bluetooth_scan.h"
#include "abi/micropixel_abi.h"

// The Host authorizes Bluetooth Classic methods by application.
#define MICROPIXEL_SERVICE_BLUETOOTH 30U
#define MICROPIXEL_BLUETOOTH_MIN_VERSION MICROPIXEL_INTERFACE_VERSION(1U, 0U)
#define MICROPIXEL_BLUETOOTH_GROUPS_VERSION MICROPIXEL_INTERFACE_VERSION(1U, 1U)
#define MICROPIXEL_BLUETOOTH_VERSION MICROPIXEL_INTERFACE_VERSION(1U, 2U)
#define MICROPIXEL_BLUETOOTH_PCM_BYTES 1024U

enum {
    MICROPIXEL_BT_STATUS = 1,
    MICROPIXEL_BT_SCAN,
    MICROPIXEL_BT_STOP_SCAN,
    MICROPIXEL_BT_DEVICE,
    MICROPIXEL_BT_SET_VISIBLE,
    MICROPIXEL_BT_CONNECT,
    MICROPIXEL_BT_DISCONNECT,
    MICROPIXEL_BT_PAIR,
    MICROPIXEL_BT_BOND,
    MICROPIXEL_BT_FORGET,
    MICROPIXEL_BT_SET_ROLE,
    MICROPIXEL_BT_OPEN_STREAM,
    MICROPIXEL_BT_START_STREAM,
    MICROPIXEL_BT_SUSPEND_STREAM,
    MICROPIXEL_BT_SET_AUDIO_ROUTE,
    MICROPIXEL_BT_READ_PCM,
    MICROPIXEL_BT_WRITE_PCM,
    MICROPIXEL_BT_REMOTE_STATUS,
    MICROPIXEL_BT_REMOTE_KEY,
};

enum { MPX_BT_UNAVAILABLE, MPX_BT_IDLE, MPX_BT_SCANNING, MPX_BT_CONNECTING, MPX_BT_CONNECTED };
enum { MPX_BT_SINK = 1, MPX_BT_SOURCE };
enum {
    MPX_BT_STREAM_IDLE,
    MPX_BT_STREAM_OPEN,
    MPX_BT_STREAM_PLAYING,
    MPX_BT_STREAM_SUSPENDED,
    MPX_BT_STREAM_CONNECTING,
};
enum { MPX_BT_AUDIO_SYSTEM, MPX_BT_AUDIO_GUEST };

enum { MPX_BT_REMOTE_IDLE, MPX_BT_REMOTE_CONNECTING, MPX_BT_REMOTE_READY, MPX_BT_REMOTE_UNAVAILABLE };
enum { MPX_BT_REMOTE_UNKNOWN, MPX_BT_REMOTE_STOPPED, MPX_BT_REMOTE_PLAYING, MPX_BT_REMOTE_PAUSED };
enum { MPX_BT_REMOTE_PLAY = 1, MPX_BT_REMOTE_PAUSE, MPX_BT_REMOTE_PREVIOUS, MPX_BT_REMOTE_NEXT };
#define MPX_BT_REMOTE_UNKNOWN_TIME UINT32_MAX

// Session changes when an AVRCP channel is closed or replaced. Time fields
// contain the peer's latest reported values in milliseconds, or UNKNOWN_TIME.
typedef struct {
    uint32_t session;
    uint32_t position_ms;
    uint32_t duration_ms;
    int32_t error;
    uint8_t link;
    uint8_t playback;
    uint8_t pending;
    uint8_t reserved;
} micropixel_bluetooth_remote_status_t;

typedef struct {
    uint32_t session;
    uint32_t key;
} micropixel_bluetooth_remote_key_t;

typedef struct {
    uint32_t revision;
    uint32_t scan_revision;
    uint32_t stream_revision;
    int32_t error;
    uint32_t sample_rate;
    uint32_t rx_sbc_frames;
    uint32_t tx_pcm_frames;
    uint32_t dropped_frames;
    uint32_t decode_errors;
    uint32_t rx_dropped_packets;
    char peer_address[18];
    uint8_t state;
    uint8_t visible;
    uint8_t role;
    uint8_t stream;
    uint8_t channels;
    uint8_t security;
    uint8_t device_count;
    uint8_t bond_count;
    uint8_t audio_route;
    uint8_t pending;
} micropixel_bluetooth_status_t;

typedef struct {
    uint32_t scan_revision;
    uint32_t index;
} micropixel_bluetooth_selection_t;

typedef struct {
    char name[33];
    char address[18];
    int8_t rssi;
    uint8_t class_of_device[3];
    uint8_t group;  // MPX_BT_DEVICE_* since 1.1. Zero on 1.0 Hosts.
} micropixel_bluetooth_device_t;

typedef struct {
    char address[18];  // ASCII XX:XX:XX:XX:XX:XX, terminated.
} micropixel_bluetooth_address_t;

// I/O is interleaved signed little-endian 16-bit PCM at the negotiated rate.
// A write request carries this header followed by complete frames. A read
// returns complete frames up to the caller's response capacity.
typedef struct {
    uint32_t stream_revision;
} micropixel_bluetooth_pcm_t;

#ifdef __cplusplus
static_assert(sizeof(micropixel_bluetooth_status_t) == 68);
static_assert(sizeof(micropixel_bluetooth_selection_t) == 8);
static_assert(sizeof(micropixel_bluetooth_device_t) == 56);
static_assert(sizeof(micropixel_bluetooth_address_t) == 18);
static_assert(sizeof(micropixel_bluetooth_pcm_t) == 4);
static_assert(sizeof(micropixel_bluetooth_remote_status_t) == 20);
static_assert(sizeof(micropixel_bluetooth_remote_key_t) == 8);
#endif
