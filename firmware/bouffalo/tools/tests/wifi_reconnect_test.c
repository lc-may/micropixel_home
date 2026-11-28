// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/network/network_time.h"
#include "platform/storage/file_store.h"
#include "wifi_history.h"
#include "wifi_reconnect_test_sdk.h"

static wifi_mgmr_scan_item_t visible[3] = {
    {.ssid = "same", .ssid_len = 4, .rssi = -35, .channel = 1, .bssid = {2, 0, 0, 0, 0, 1}},
    {.ssid = "same",
     .ssid_len = 4,
     .rssi = -70,
     .channel = 44,
     .bssid = {2, 0, 0, 0, 0, 2},
     .auth = WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK},
    {.ssid = "same",
     .ssid_len = 4,
     .rssi = -15,
     .channel = 6,
     .bssid = {2, 0, 0, 0, 0, 3},
     .auth = WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK},
};
static unsigned visible_count = 3;
static unsigned scans, joins, leaves, static_restores;
static int connect_error;
static uint8_t target;
static wifi_mgmr_sta_connect_params_t connection;
static struct net_al_ext_ip_addr_cfg ip_config;
static void (*wifi_callback)(async_input_event_t, void*);

static void event(unsigned code) {
    struct async_input_event event = {.code = code};
    wifi_callback(&event, NULL);
}

static mpx_wireless_status_t wireless_status(void) {
    mpx_wireless_status_t state;
    assert(mpx_wireless_read(&state, 0));
    return state;
}

int async_register_event_filter(uintptr_t type, void (*callback)(async_input_event_t, void*), void* context) {
    assert(type == EV_WIFI && !context);
    wifi_callback = callback;
    return 0;
}
int wifi_mgmr_sta_autoconnect_disable(void) { return 0; }
int wifi_mgmr_sta_scan(const wifi_mgmr_scan_params_t* params) {
    assert(params);
    ++scans;
    return 0;
}
unsigned wifi_mgmr_sta_scanlist_dump(void* output, unsigned capacity) {
    assert(capacity >= visible_count);
    memcpy(output, visible, sizeof(*visible) * visible_count);
    return visible_count;
}
int wifi_mgmr_sta_connect(const wifi_mgmr_sta_connect_params_t* params) {
    ++joins;
    connection = *params;
    assert(connection.bssid_str[16] >= '1' && connection.bssid_str[16] <= '3');
    target = connection.bssid_str[16] - '1';
    ip_config = (struct net_al_ext_ip_addr_cfg){0};
    return connect_error;
}
int wifi_mgmr_sta_connect_ind_stat_get(wifi_mgmr_connect_ind_stat_info_t* info) {
    memcpy(info->ssid, connection.ssid, connection.ssid_len);
    memcpy(info->passphr, connection.key, connection.key_len);
    memcpy(info->bssid, visible[target].bssid, sizeof(info->bssid));
    info->channel = visible[target].channel;
    return 0;
}
int wifi_sta_disconnect(void) {
    ++leaves;
    return 0;
}
int wifi_sta_ip4_addr_get(uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* dns) {
    assert(!mask && !gw && !dns);
    *ip = ip_config.ipv4.addr;
    return 0;
}
int wifi_mgmr_sta_ip_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    ++static_restores;
    ip_config = (struct net_al_ext_ip_addr_cfg){.mode = IP_ADDR_STATIC_IPV4, .ipv4 = {ip, mask, gw, dns}};
    return 0;
}
int net_al_ext_get_vif_ip(int vif, struct net_al_ext_ip_addr_cfg* config) {
    assert(vif == MGMR_VIF_STA);
    *config = ip_config;
    return 0;
}
void mpx_network_time_start(void) {}
size_t lfs_kv_get_blob(const char* key, void* out, size_t capacity, size_t* stored) {
    (void)key;
    (void)out;
    (void)capacity;
    *stored = 0;
    return 0;
}
void* kmalloc(size_t size, unsigned flags) {
    assert(flags == (MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
    return malloc(size);
}
void kfree(void* address) { free(address); }

bool mpx_files_mount(void) { return true; }
int mpx_file_read(const char* name, void* out, size_t capacity, size_t* length) {
    *length = 0;
    FILE* file = fopen(name, "rb");
    if (!file) return -ENOENT;
    assert(fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file);
    assert(size >= 0);
    *length = size;
    const int result = *length > capacity ? -ENOSPC : 0;
    if (!result && *length) {
        rewind(file);
        assert(fread(out, 1, *length, file) == *length);
    }
    assert(fclose(file) == 0);
    return result;
}
int mpx_file_write(const char* name, const void* data, size_t length) {
    FILE* file = fopen(name, "wb");
    assert(file && fwrite(data, 1, length, file) == length);
    assert(fclose(file) == 0);
    return 0;
}

static void scan_manually(uint32_t now) {
    assert(mpx_wireless_command(MICROPIXEL_WIRELESS_WIFI_SCAN, 0, 0, NULL, 0) == 0);
    mpx_wireless_poll(now);
    assert(wireless_status().wifi_scanning);
    event(CODE_WIFI_ON_SCAN_DONE);
    mpx_wireless_poll(now + 1);
    assert(!wireless_status().wifi_scanning && wireless_status().wifi_count == visible_count);
}

static void join_manually(uint32_t now, uint32_t index, const char* password) {
    const uint32_t revision = wireless_status().wifi_scan_revision;
    assert(mpx_wireless_command(MICROPIXEL_WIRELESS_WIFI_JOIN, index, revision, password, strlen(password)) == 0);
    mpx_wireless_poll(now);
    assert(wireless_status().wifi == MPX_WIFI_JOINING);
}

static void train(void) {
    scan_manually(1);
    join_manually(3, 1, "");  // The open network has the middle RSSI.
    assert(target == 0);
    event(CODE_WIFI_ON_CONNECTED);
    ip_config.mode = IP_ADDR_DHCP_CLIENT;
    ip_config.ipv4.addr = 1;
    event(CODE_WIFI_ON_GOT_IP);
    mpx_wireless_poll(4);
    assert(mpx_wifi_online());

    assert(mpx_wireless_command(MICROPIXEL_WIRELESS_WIFI_LEAVE, 0, 0, NULL, 0) == 0);
    mpx_wireless_poll(5);
    assert(leaves == 1);
    event(CODE_WIFI_ON_DISCONNECT);
    mpx_wireless_poll(6);
    assert(scans == 1);
    scan_manually(7);
    join_manually(9, 2, "example-pass");
    assert(target == 1);
    event(CODE_WIFI_ON_CONNECTED);
    assert(wifi_mgmr_sta_ip_set(2, 3, 4, 5) == 0);
    event(CODE_WIFI_ON_GOT_IP);
    mpx_wireless_poll(10);
    assert(mpx_wifi_history_profile(0)->channel == 44);
    assert(mpx_wifi_history_profile(1)->channel == 1);
    assert(!mpx_wifi_history_profile(2));
}

static void reconnect(void) {
    mpx_wireless_poll(1);
    assert(scans == 1 && joins == 0 && !wireless_status().wifi_scanning);
    visible[1].channel = 48;
    event(CODE_WIFI_ON_SCAN_DONE);
    mpx_wireless_poll(2);
    assert(joins == 1 && target == 1 && connection.freq1 == 5240 && !connection.use_dhcp);
    assert(connection.key_len == strlen("example-pass") && !memcmp(connection.key, "example-pass", connection.key_len));
    event(CODE_WIFI_ON_CONNECTED);
    mpx_wireless_poll(3);
    assert(static_restores == 1 && ip_config.ipv4.addr == 2 && ip_config.ipv4.dns == 5);

    // IP acquisition failure waits for the driver to disconnect before trying the next saved AP.
    event(CODE_WIFI_ON_GOT_IP_TIMEOUT);
    mpx_wireless_poll(4);
    assert(joins == 1 && leaves == 1);
    event(CODE_WIFI_ON_DISCONNECT);
    mpx_wireless_poll(5);
    assert(joins == 2 && target == 0 && connection.use_dhcp && !connection.key_len);
    event(CODE_WIFI_ON_CONNECTED);
    ip_config.mode = IP_ADDR_DHCP_CLIENT;
    ip_config.ipv4.addr = 1;
    event(CODE_WIFI_ON_GOT_IP);
    event(CODE_WIFI_ON_DISCONNECT);
    ip_config.ipv4.addr = 0;
    event(CODE_WIFI_ON_LOST_IP);  // A stale IP event must not leave the controller waiting for DHCP.
    mpx_wireless_poll(7);
    assert(mpx_wifi_history_profile(0)->bssid[5] == 1);
    assert(mpx_wifi_history_profile(1)->bssid[5] == 2);
    assert(scans == 2);
    scan_manually(8);  // Promote the active background scan and give the user priority.
    assert(scans == 2 && joins == 2);
    mpx_wireless_poll(60009);
    assert(scans == 3);
    connect_error = -EBUSY;
    event(CODE_WIFI_ON_SCAN_DONE);
    const uint32_t scan_completed_ms = 60010;
    mpx_wireless_poll(scan_completed_ms);
    assert(joins == 3 && target == 0);
    connect_error = 0;
    mpx_wireless_poll(scan_completed_ms + 1);
    assert(joins == 4 && target == 1);
    event(CODE_WIFI_ON_DISCONNECT);
    visible_count = 0;
    const uint32_t retry_ms = 120000;
    const uint32_t next_scan_ms = scan_completed_ms + retry_ms;
    mpx_wireless_poll(next_scan_ms - 1);
    assert(scans == 3);
    mpx_wireless_poll(next_scan_ms);
    assert(scans == 4);
    event(CODE_WIFI_ON_SCAN_DONE);
    const uint32_t empty_scan_completed_ms = next_scan_ms + 1;
    mpx_wireless_poll(empty_scan_completed_ms);
    assert(scans == 4 && joins == 4);
    mpx_wireless_poll(empty_scan_completed_ms + retry_ms - 1);
    assert(scans == 4 && joins == 4);
    mpx_wireless_poll(empty_scan_completed_ms + retry_ms);
    assert(scans == 5 && joins == 4);
}

static void migrate(void) {
    const struct {
        uint8_t version, ip_mode, bssid[6];
        char ssid[33], password[65];
    } legacy = {.version = 1, .ip_mode = IP_ADDR_DHCP_CLIENT, .ssid = "same", .bssid = {2, 0, 0, 0, 0, 1}};
    assert(mpx_file_write("settings.wifi", &legacy, sizeof(legacy)) == 0);
    mpx_wireless_init();
    assert(mpx_wifi_history_profile(0)->bssid[5] == 1);
    assert(!mpx_wifi_history_profile(1));
    event(CODE_WIFI_ON_MGMR_DONE);
    mpx_wireless_poll(1);
    event(CODE_WIFI_ON_SCAN_DONE);
    mpx_wireless_poll(2);
    assert(scans == 1 && joins == 1 && target == 0);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "migrate")) {
        migrate();
    } else {
        mpx_wireless_init();
        event(CODE_WIFI_ON_MGMR_DONE);
        if (!strcmp(argv[1], "train"))
            train();
        else
            reconnect();
    }
}
