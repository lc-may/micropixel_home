// SPDX-License-Identifier: Apache-2.0
#pragma once

#define MPX_BLE_SMOKE_SERVICE_UUID BT_UUID_128_ENCODE(0x79180b9c, 0xf9dc, 0x47b2, 0xa71b, 0x4cd98f201201)

int mpx_ble_smoke_init(void);
void mpx_ble_smoke_poll(void);
