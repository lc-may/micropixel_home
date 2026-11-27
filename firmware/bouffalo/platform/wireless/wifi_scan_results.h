// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "wireless.h"

// Sort all APs by RSSI, then determine the join targets within each SSID.
void mpx_wifi_prepare_results(mpx_wifi_network_t* networks, unsigned count);
