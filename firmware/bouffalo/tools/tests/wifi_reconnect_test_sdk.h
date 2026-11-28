// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATTR_NOINIT_PSRAM_SECTION
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define WIFI_STA_DHCPC_TIMEOUT_MS_DEFAULT 64000

enum {
    EV_WIFI,
    CODE_WIFI_ON_MGMR_DONE,
    CODE_WIFI_ON_CONNECTING,
    CODE_WIFI_ON_CONNECTED,
    CODE_WIFI_ON_GOT_IP,
    CODE_WIFI_ON_LOST_IP,
    CODE_WIFI_ON_GOT_IP_TIMEOUT,
    CODE_WIFI_ON_PARAMS_ERROR,
    CODE_WIFI_ON_DISCONNECT,
    CODE_WIFI_ON_SCAN_DONE,
};
enum { IP_ADDR_NONE, IP_ADDR_STATIC_IPV4, IP_ADDR_DHCP_CLIENT, MGMR_VIF_STA };
enum {
    WIFI_EVENT_BEACON_IND_AUTH_OPEN,
    WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK,
    WIFI_EVENT_BEACON_IND_AUTH_WPA_WPA2_PSK,
    WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK_WPA3_SAE,
    WIFI_EVENT_BEACON_IND_AUTH_WPA3_SAE,
};
enum { MM_FLAG_HEAP_PSRAM_0 = 1, MM_FLAG_ALIGN_8BYTE = 2 };

typedef struct async_input_event {
    uint16_t code;
}* async_input_event_t;
typedef struct {
    char ssid[33], passphr[65];
    uint8_t bssid[6], channel;
} wifi_mgmr_connect_ind_stat_info_t;
typedef struct {
    int ssid_len;
    char ssid[32];
    int8_t rssi;
    uint8_t channel, auth, bssid[6];
} wifi_mgmr_scan_item_t;
typedef struct {
    uint8_t reserved;
} wifi_mgmr_scan_params_t;
typedef struct {
    char ssid[32], key[64], bssid_str[18];
    uint8_t ssid_len, key_len, use_dhcp, dhcp_timeout_event_only;
    uint16_t freq1;
} wifi_mgmr_sta_connect_params_t;
struct net_al_ext_ip_addr_cfg {
    uint8_t mode;
    struct {
        uint32_t addr, mask, gw, dns;
    } ipv4;
};

int async_register_event_filter(uintptr_t type, void (*callback)(async_input_event_t, void*), void* context);
int wifi_mgmr_sta_autoconnect_disable(void);
int wifi_mgmr_sta_scan(const wifi_mgmr_scan_params_t* params);
unsigned wifi_mgmr_sta_scanlist_dump(void* output, unsigned capacity);
int wifi_mgmr_sta_connect(const wifi_mgmr_sta_connect_params_t* params);
int wifi_mgmr_sta_connect_ind_stat_get(wifi_mgmr_connect_ind_stat_info_t* info);
int wifi_sta_disconnect(void);
int wifi_sta_ip4_addr_get(uint32_t* ip, uint32_t* mask, uint32_t* gw, uint32_t* dns);
int wifi_mgmr_sta_ip_set(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);
int net_al_ext_get_vif_ip(int vif, struct net_al_ext_ip_addr_cfg* config);
size_t lfs_kv_get_blob(const char* key, void* out, size_t capacity, size_t* stored);
void* kmalloc(size_t size, unsigned flags);
void kfree(void* address);
