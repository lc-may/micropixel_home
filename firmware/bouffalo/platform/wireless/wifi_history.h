// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "wireless.h"

typedef struct {
    uint32_t addr, mask, gateway, dns;
} mpx_wifi_ipv4_t;

typedef struct {
    mpx_wifi_ipv4_t ipv4;  // Zero for DHCP.
    char ssid[33];
    char password[65];
    uint8_t bssid[6];
    uint8_t channel;
    uint8_t reserved[3];
} mpx_wifi_profile_t;

// Owned by the wireless controller. A profile pointer is valid until the next remember call.
bool mpx_wifi_history_load(void);
bool mpx_wifi_history_remember(const mpx_wifi_profile_t* profile);
void mpx_wifi_history_flush(void);
const mpx_wifi_profile_t* mpx_wifi_history_profile(size_t index);
const mpx_wifi_network_t* mpx_wifi_profile_match(const mpx_wifi_profile_t* profile, const mpx_wifi_network_t* networks,
                                                 unsigned count);
bool mpx_wifi_profile_credentials_valid(const mpx_wifi_profile_t* profile);
bool mpx_wifi_unicast_bssid(const uint8_t bssid[6]);
void mpx_wifi_clear_credentials(void* value, size_t size);
