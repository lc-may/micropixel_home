// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "network_time.h"

#define SNTP_SERVER_DNS 1
#define SNTP_MAX_SERVERS 2
#define SNTP_CHECK_RESPONSE 2
#define SNTP_SET_SYSTEM_TIME(seconds) mpx_network_set_time(seconds)
#define SNTP_GET_SYSTEM_TIME(seconds, microseconds) mpx_network_get_time(&(seconds), &(microseconds))
