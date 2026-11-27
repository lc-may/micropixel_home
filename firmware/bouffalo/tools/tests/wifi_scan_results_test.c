// SPDX-License-Identifier: Apache-2.0
#include "wifi_scan_results.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void SetNetwork(mpx_wifi_network_t* item, uint8_t id, const char* ssid, uint8_t channel, int8_t rssi, bool open,
                       bool supported) {
    *item = (mpx_wifi_network_t){
        .length = strlen(ssid), .open = open, .supported = supported, .rssi = rssi, .channel = channel, .bssid = {2}};
    memcpy(item->ssid, ssid, item->length);
    item->bssid[5] = id;
}

int main(void) {
    mpx_wifi_network_t results[MPX_WIFI_RESULTS];
    for (unsigned i = 0; i < MPX_WIFI_RESULTS; ++i) {
        char ssid[33];
        snprintf(ssid, sizeof(ssid), "Network %u", i);
        SetNetwork(&results[i], i, ssid, 6, -30 - (int)(i % 60), false, true);
    }
    SetNetwork(&results[0], 0, "Mesh", 6, -25, false, true);
    SetNetwork(&results[1], 1, "Mesh", 149, -57, false, true);
    SetNetwork(&results[2], 2, "Mesh", 36, -49, false, true);
    SetNetwork(&results[3], 3, "Mesh", 44, -32, true, true);
    SetNetwork(&results[4], 4, "Mesh", 153, -20, false, false);
    SetNetwork(&results[5], 5, "Mesh", 11, -35, false, true);
    SetNetwork(&results[6], 6, "Legacy", 11, -37, false, true);
    SetNetwork(&results[7], 7, "Legacy", 149, -42, false, false);
    SetNetwork(&results[8], 8, "", 36, -47, false, false);
    SetNetwork(&results[9], 9, "Mesh other", 11, -10, false, true);
    SetNetwork(&results[10], 10, "Public", 1, -21, true, true);
    SetNetwork(&results[11], 11, "Public", 44, -60, false, true);

    mpx_wifi_prepare_results(results, 0);
    mpx_wifi_prepare_results(results, MPX_WIFI_RESULTS);

    unsigned indexes[MPX_WIFI_RESULTS];
    bool seen[MPX_WIFI_RESULTS] = {false};
    for (unsigned i = 0; i < MPX_WIFI_RESULTS; ++i) {
        assert(i == 0 || results[i - 1].rssi >= results[i].rssi);
        const unsigned id = results[i].bssid[5];
        assert(id < MPX_WIFI_RESULTS && !seen[id]);
        seen[id] = true;
        indexes[id] = i;
        assert(results[i].preferred < MPX_WIFI_RESULTS);
        assert(results[i].fallback == MPX_WIFI_NO_INDEX || results[i].fallback < MPX_WIFI_RESULTS);
    }
    assert(indexes[9] == 0);
    assert(indexes[12] < indexes[72]);  // Equal RSSI retains scan order.

    const uint8_t dual = MPX_WIFI_BAND_2_4_GHZ | MPX_WIFI_BAND_5_GHZ;
    assert(results[indexes[0]].ssid_bands == dual && results[indexes[2]].ssid_bands == dual);
    assert(results[indexes[0]].preferred == indexes[2] && results[indexes[0]].fallback == indexes[0]);
    assert(results[indexes[1]].preferred == indexes[2] && results[indexes[1]].fallback == indexes[0]);
    assert(results[indexes[3]].preferred == indexes[3] && results[indexes[3]].fallback == MPX_WIFI_NO_INDEX);
    assert(results[indexes[4]].preferred == indexes[4] && results[indexes[4]].fallback == MPX_WIFI_NO_INDEX);
    assert(results[indexes[6]].ssid_bands == dual && results[indexes[6]].preferred == indexes[6]);
    assert(results[indexes[8]].ssid_bands == MPX_WIFI_BAND_5_GHZ && !results[indexes[8]].supported);
    assert(results[indexes[10]].preferred == indexes[10] && results[indexes[11]].fallback == MPX_WIFI_NO_INDEX);
    return 0;
}
