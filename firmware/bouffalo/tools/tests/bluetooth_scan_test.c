// SPDX-License-Identifier: Apache-2.0
#include "bluetooth_scan.h"

#include <assert.h>
#include <string.h>

int main(void) {
    mpx_bt_scan_data_t scan = {0};
    const uint8_t hid_ad[] = {3, 0x02, 0x12, 0x18, 2, 0x08, 'M'};
    mpx_bt_scan_parse(&scan, hid_ad, sizeof(hid_ad));
    assert(mpx_bt_scan_le_group(&scan) == MPX_BT_DEVICE_INPUT);
    assert(strcmp(scan.name, "M") == 0);

    const uint8_t mouse_response[] = {3, 0x19, 0xc2, 0x03, 6, 0x09, 'M', 'o', 'u', 's', 'e'};
    mpx_bt_scan_parse(&scan, mouse_response, sizeof(mouse_response));
    mpx_bt_scan_parse(&scan, hid_ad, sizeof(hid_ad));
    assert(scan.appearance == 0x03c2 && strcmp(scan.name, "Mouse") == 0);
    assert(mpx_bt_scan_le_group(&scan) == MPX_BT_DEVICE_INPUT);

    const uint8_t gamepad_response[] = {3, 0x19, 0xc4, 0x03};
    mpx_bt_scan_parse(&scan, gamepad_response, sizeof(gamepad_response));
    assert(mpx_bt_scan_le_group(&scan) == MPX_BT_DEVICE_OTHER);

    const uint8_t keyboard_cod[] = {0x40, 0x05, 0x00};
    const uint8_t pen_cod[] = {0x1c, 0x05, 0x00};
    const uint8_t gamepad_cod[] = {0x08, 0x05, 0x00};
    const uint8_t headset_cod[] = {0x04, 0x04, 0x20};
    const uint8_t computer_cod[] = {0x0c, 0x01, 0x20};
    const uint8_t unknown_cod[] = {0x00, 0x1f, 0x00};
    assert(mpx_bt_scan_br_group(&scan, keyboard_cod) == MPX_BT_DEVICE_INPUT);
    assert(mpx_bt_scan_br_group(&scan, pen_cod) == MPX_BT_DEVICE_INPUT);
    assert(mpx_bt_scan_br_group(&scan, gamepad_cod) == MPX_BT_DEVICE_OTHER);
    assert(mpx_bt_scan_br_group(&scan, headset_cod) == MPX_BT_DEVICE_AUDIO);

    scan = (mpx_bt_scan_data_t){0};
    const uint8_t audio_eir[] = {3, 0x03, 0x0b, 0x11};
    mpx_bt_scan_parse(&scan, audio_eir, sizeof(audio_eir));
    assert(mpx_bt_scan_br_group(&scan, unknown_cod) == MPX_BT_DEVICE_AUDIO);
    assert(mpx_bt_scan_br_group(&scan, computer_cod) == MPX_BT_DEVICE_OTHER);

    scan = (mpx_bt_scan_data_t){0};
    const uint8_t audio_service_data[] = {4, 0x16, 0x52, 0x18, 0x00};
    mpx_bt_scan_parse(&scan, audio_service_data, sizeof(audio_service_data));
    assert(mpx_bt_scan_le_group(&scan) == MPX_BT_DEVICE_AUDIO);

    scan = (mpx_bt_scan_data_t){0};
    const uint8_t appearance_only[] = {3, 0x19, 0x41, 0x08};
    mpx_bt_scan_parse(&scan, appearance_only, sizeof(appearance_only));
    assert(mpx_bt_scan_le_group(&scan) == MPX_BT_DEVICE_AUDIO);

    scan = (mpx_bt_scan_data_t){0};
    const uint8_t heart_rate_ad[] = {3, 0x03, 0x0d, 0x18, 6, 0x09, 'B', 'a'};
    mpx_bt_scan_parse(&scan, heart_rate_ad, sizeof(heart_rate_ad));
    assert(mpx_bt_scan_le_group(&scan) == MPX_BT_DEVICE_OTHER);
    assert(scan.name[0] == 0);
}
