// SPDX-License-Identifier: Apache-2.0
#include "network_time.h"

#include <stdio.h>
#include <sys/time.h>

#include "lwip/apps/sntp.h"
#include "lwip/tcpip.h"

static bool time_valid(time_t seconds) { return seconds >= 1704067200 && (uint64_t)seconds < 4102444800ULL; }

void mpx_network_set_time(uint32_t seconds) {
    if (time_valid((time_t)seconds)) {
        const struct timeval now = {.tv_sec = seconds};
        if (settimeofday(&now, NULL) == 0) puts("MicroPixel network clock synchronized");
    }
}

void mpx_network_get_time(uint32_t* seconds, uint32_t* microseconds) {
    struct timeval now = {0};
    gettimeofday(&now, NULL);
    *seconds = now.tv_sec;
    *microseconds = now.tv_usec;
}

bool mpx_network_time_ready(void) {
    struct timeval now = {0};
    return gettimeofday(&now, NULL) == 0 && time_valid(now.tv_sec);
}

static void start_sntp(void* unused) {
    (void)unused;
    sntp_setservername(0, "ntp.aliyun.com");
#if SNTP_MAX_SERVERS > 1
    sntp_setservername(1, "time.cloudflare.com");
#endif
    sntp_init();
}

void mpx_network_time_start(void) {
    static bool scheduled;
    if (!scheduled) scheduled = tcpip_try_callback(start_sntp, NULL) == ERR_OK;
}
