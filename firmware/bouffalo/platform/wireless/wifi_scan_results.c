// SPDX-License-Identifier: Apache-2.0
#include "wifi_scan_results.h"

#include <string.h>

_Static_assert(MPX_WIFI_RESULTS < MPX_WIFI_NO_INDEX, "Wi-Fi result indexes must fit in a byte");

static uint8_t wifi_band(uint8_t channel) {
    if (channel >= 1 && channel <= 14) return MPX_WIFI_BAND_2_4_GHZ;
    if (channel >= 32 && channel <= 196) return MPX_WIFI_BAND_5_GHZ;
    return 0;
}

void mpx_wifi_prepare_results(mpx_wifi_network_t* networks, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {
        const mpx_wifi_network_t item = networks[i];
        unsigned index = i;
        while (index > 0 && networks[index - 1].rssi < item.rssi) {
            networks[index] = networks[index - 1];
            --index;
        }
        networks[index] = item;
    }

    for (unsigned i = 0; i < count; ++i) {
        mpx_wifi_network_t* item = &networks[i];
        item->ssid_bands = wifi_band(item->channel);
        item->preferred = i;
        item->fallback = MPX_WIFI_NO_INDEX;
        if (item->length == 0) continue;

        uint8_t best_5_ghz = MPX_WIFI_NO_INDEX;
        uint8_t best_2_4_ghz = MPX_WIFI_NO_INDEX;
        for (unsigned j = 0; j < count; ++j) {
            const mpx_wifi_network_t* candidate = &networks[j];
            if (candidate->length != item->length || memcmp(candidate->ssid, item->ssid, item->length)) continue;
            const uint8_t band = wifi_band(candidate->channel);
            item->ssid_bands |= band;
            if (!item->supported || !candidate->supported || item->open != candidate->open) continue;
            if (band == MPX_WIFI_BAND_5_GHZ && best_5_ghz == MPX_WIFI_NO_INDEX) best_5_ghz = j;
            if (band == MPX_WIFI_BAND_2_4_GHZ && best_2_4_ghz == MPX_WIFI_NO_INDEX) best_2_4_ghz = j;
        }
        if (best_5_ghz != MPX_WIFI_NO_INDEX) {
            item->preferred = best_5_ghz;
            item->fallback = best_2_4_ghz;
        }
    }
}
