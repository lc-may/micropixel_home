// SPDX-License-Identifier: Apache-2.0
#include "bluetooth_scan.h"

#include <string.h>

// Advertising data types, service UUIDs and device classes are from the
// Bluetooth SIG Assigned Numbers.
enum {
    UUID16_INCOMPLETE = 0x02,
    UUID16_COMPLETE = 0x03,
    NAME_SHORTENED = 0x08,
    NAME_COMPLETE = 0x09,
    SERVICE_DATA16 = 0x16,
    APPEARANCE = 0x19,
};

static uint16_t read_le16(const uint8_t* data) { return (uint16_t)data[0] | ((uint16_t)data[1] << 8); }

static uint8_t uuid_group(uint16_t uuid) {
    switch (uuid) {
        case 0x1124:  // HID
        case 0x1812:  // HID over GATT
            return 1U << MPX_BT_DEVICE_INPUT;
        case 0x1108:  // Headset
        case 0x110a:  // Audio Source
        case 0x110b:  // Audio Sink
        case 0x110d:  // Advanced Audio Distribution
        case 0x111e:  // Hands-Free
        case 0x184e:  // Audio Stream Control
        case 0x1850:  // Published Audio Capabilities
        case 0x1852:  // Broadcast Audio Announcement
        case 0x1854:  // Hearing Access
            return 1U << MPX_BT_DEVICE_AUDIO;
        default:
            return 0;
    }
}

void mpx_bt_scan_parse(mpx_bt_scan_data_t* scan, const uint8_t* data, size_t length) {
    while (length) {
        const size_t field_length = *data++;
        --length;
        if (!field_length || field_length > length) break;
        const uint8_t type = *data++;
        const size_t size = field_length - 1;
        if ((type == NAME_COMPLETE || (type == NAME_SHORTENED && !scan->complete_name)) && size && *data) {
            const size_t count = size < sizeof(scan->name) ? size : sizeof(scan->name) - 1;
            memcpy(scan->name, data, count);
            scan->name[count] = 0;
            scan->complete_name = type == NAME_COMPLETE;
        } else if (type == APPEARANCE && size == sizeof(uint16_t)) {
            const uint16_t appearance = read_le16(data);
            if (appearance) scan->appearance = appearance;
        } else if (type == UUID16_INCOMPLETE || type == UUID16_COMPLETE) {
            for (size_t i = 0; i + 1 < size; i += 2) scan->service_groups |= uuid_group(read_le16(data + i));
        } else if (type == SERVICE_DATA16 && size >= sizeof(uint16_t)) {
            scan->service_groups |= uuid_group(read_le16(data));
        }
        data += size;
        length -= field_length;
    }
}

static uint8_t service_group(const mpx_bt_scan_data_t* scan) {
    if (scan->service_groups & (1U << MPX_BT_DEVICE_AUDIO)) return MPX_BT_DEVICE_AUDIO;
    if (scan->service_groups & (1U << MPX_BT_DEVICE_INPUT)) return MPX_BT_DEVICE_INPUT;
    return MPX_BT_DEVICE_OTHER;
}

uint8_t mpx_bt_scan_le_group(const mpx_bt_scan_data_t* scan) {
    switch (scan->appearance >> 6) {
        case 0x0f:  // Human Interface Device
            switch (scan->appearance & 0x3f) {
                case 0x00:  // Generic HID
                case 0x01:  // Keyboard
                case 0x02:  // Mouse
                case 0x05:  // Digitizer Tablet
                case 0x07:  // Digital Pen
                case 0x09:  // Touchpad
                    return MPX_BT_DEVICE_INPUT;
                default:
                    return MPX_BT_DEVICE_OTHER;
            }
        case 0x21:  // Audio Sink
        case 0x22:  // Audio Source
        case 0x25:  // Wearable Audio Device
        case 0x29:  // Hearing Aid
            return MPX_BT_DEVICE_AUDIO;
        default:
            return service_group(scan);
    }
}

uint8_t mpx_bt_scan_br_group(const mpx_bt_scan_data_t* scan, const uint8_t cod[3]) {
    if ((cod[0] & 0x03) != 0) return service_group(scan);
    const uint8_t major = cod[1] & 0x1f;
    if (major == 0x04) return MPX_BT_DEVICE_AUDIO;  // Audio/Video
    if (major == 0x05) {                            // Peripheral
        const uint8_t kind = (cod[0] >> 2) & 0x0f;
        if ((cod[0] & 0xc0) || kind == 5 || kind == 7) return MPX_BT_DEVICE_INPUT;
        if (kind != 0) return MPX_BT_DEVICE_OTHER;
    } else if (major != 0x00 && major != 0x1f) {
        return MPX_BT_DEVICE_OTHER;
    }
    return service_group(scan);
}
