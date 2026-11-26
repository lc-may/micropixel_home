// SPDX-License-Identifier: Apache-2.0
#include "network_time.h"

#include <stdio.h>
#include <sys/time.h>

#include "bflb_mtimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"
#include "lwip/tcpip.h"

static mpx_network_time_sync_t pending_sync;

static bool time_valid(time_t seconds) { return seconds >= 1704067200 && (uint64_t)seconds < 4102444800ULL; }

void mpx_network_set_time(uint32_t seconds, uint32_t microseconds) {
    if (!time_valid((time_t)seconds) || microseconds >= 1000000U) return;
    const struct timeval now = {.tv_sec = seconds, .tv_usec = microseconds};
    const uint64_t monotonic_ms = bflb_mtimer_get_time_us() / 1000U;
    if (settimeofday(&now, NULL) != 0) return;
    taskENTER_CRITICAL();
    pending_sync =
        (mpx_network_time_sync_t){.monotonic_ms = monotonic_ms, .seconds = seconds, .microseconds = microseconds};
    taskEXIT_CRITICAL();
    puts("MicroPixel network clock synchronized");
}

bool mpx_network_time_take_sync(mpx_network_time_sync_t* sync) {
    taskENTER_CRITICAL();
    const bool available = pending_sync.seconds != 0;
    if (available) {
        *sync = pending_sync;
        pending_sync.seconds = 0;
    }
    taskEXIT_CRITICAL();
    return available;
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
