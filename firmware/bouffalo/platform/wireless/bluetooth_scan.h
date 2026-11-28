// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/bluetooth_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[33];
    uint16_t appearance;
    uint8_t service_groups;
    bool complete_name;
} mpx_bt_scan_data_t;

// Merge Bluetooth advertising data, scan responses or BR/EDR EIR.
void mpx_bt_scan_parse(mpx_bt_scan_data_t* scan, const uint8_t* data, size_t length);
uint8_t mpx_bt_scan_le_group(const mpx_bt_scan_data_t* scan);
uint8_t mpx_bt_scan_br_group(const mpx_bt_scan_data_t* scan, const uint8_t cod[3]);

#ifdef __cplusplus
}
#endif
