// SPDX-License-Identifier: Apache-2.0
#include "wireless.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#if defined(CONFIG_FHOST)
#include "async_event.h"
#include "lfs_kv.h"
#include "net_al.h"
#include "net_al_ext.h"
#include "platform/network/network_time.h"
#include "platform/storage/file_store.h"
#include "wifi_mgmr_ext.h"
#endif
#if defined(CONFIG_COEX)
#include "wifi_mgmr_coex.h"
#endif
#if defined(CONFIG_BLUETOOTH)
#include "bluetooth.h"
#include "btble_lib_api.h"
#include "conn.h"
#include "hci_driver.h"
#include "hci_err.h"
#endif

static mpx_wireless_status_t status = {.revision = 1};

#if defined(CONFIG_FHOST)
static wifi_mgmr_scan_item_t wifi_scan_cache[MPX_WIFI_RESULTS * 2];
static bool wifi_scan_done;
static bool wifi_join_pending;
static bool wifi_restore_pending;
static bool wifi_save_pending;
static bool wifi_restore_static;
static bool wifi_apply_static;
static const char wifi_profile_file[] = "settings.wifi";
enum { WIFI_PROFILE_VERSION = 1 };
typedef struct {
    uint8_t version;
    uint8_t ip_mode;
    uint8_t bssid[6];
    char ssid[33];
    char password[65];
} wifi_profile_t;
typedef struct {
    uint32_t addr, mask, gateway, dns;
} wifi_static_ipv4_t;
static wifi_profile_t saved_wifi;
static wifi_static_ipv4_t saved_ipv4;
static uint32_t next_profile_check_ms;
static bool profile_exists;
static void clear_credentials(void* value, size_t size) {
    for (volatile unsigned char* p = value; size; --size) *p++ = 0;
}

static bool credentials_valid(const wifi_profile_t* credentials) {
    const size_t ssid_length = strnlen(credentials->ssid, sizeof(credentials->ssid));
    const size_t password_length = strnlen(credentials->password, sizeof(credentials->password));
    return ssid_length > 0 && ssid_length <= 32 &&
           (!password_length || (password_length >= 8 && password_length <= 63));
}

static bool read_config_string(const char* key, char* value, size_t capacity) {
    size_t stored = 0;
    const size_t count = lfs_kv_get_blob(key, value, capacity, &stored);
    return count && count == stored && memchr(value, 0, count) != NULL;
}

static bool unicast_bssid(const uint8_t bssid[6]) {
    static const uint8_t zero[6];
    return !(bssid[0] & 1) && memcmp(bssid, zero, sizeof(zero)) != 0;
}

static bool read_profile(void) {
    uint8_t data[sizeof(saved_wifi) + sizeof(saved_ipv4)];
    size_t size = 0;
    if (mpx_file_read(wifi_profile_file, data, sizeof(data), &size) || size < sizeof(saved_wifi)) {
        clear_credentials(data, sizeof(data));
        return false;
    }
    memcpy(&saved_wifi, data, sizeof(saved_wifi));
    if (saved_wifi.ip_mode == IP_ADDR_STATIC_IPV4 && size == sizeof(data))
        memcpy(&saved_ipv4, data + sizeof(saved_wifi), sizeof(saved_ipv4));
    clear_credentials(data, sizeof(data));
    if (saved_wifi.version != WIFI_PROFILE_VERSION || !unicast_bssid(saved_wifi.bssid) ||
        !credentials_valid(&saved_wifi))
        return false;
    if (saved_wifi.ip_mode == IP_ADDR_STATIC_IPV4) return size == sizeof(data) && saved_ipv4.addr && saved_ipv4.mask;
    return saved_wifi.ip_mode == IP_ADDR_DHCP_CLIENT && size == sizeof(saved_wifi);
}

static bool write_profile(const wifi_profile_t* credentials, const wifi_static_ipv4_t* address) {
    uint8_t data[sizeof(*credentials) + sizeof(*address)];
    size_t size = sizeof(*credentials);
    memcpy(data, credentials, size);
    if (credentials->ip_mode == IP_ADDR_STATIC_IPV4) {
        memcpy(data + size, address, sizeof(*address));
        size += sizeof(*address);
    }
    const int result = mpx_file_write(wifi_profile_file, data, size);
    clear_credentials(data, sizeof(data));
    return result == 0;
}

static void load_wifi_credentials(void) {
    if (!mpx_files_mount()) puts("MicroPixel Wi-Fi profile filesystem unavailable");
    profile_exists = read_profile();
    if (profile_exists) return;
    // Migrate a working Timebox profile on the next successful connection.
    clear_credentials(&saved_wifi, sizeof(saved_wifi));
    saved_wifi.version = WIFI_PROFILE_VERSION;
    saved_wifi.ip_mode = IP_ADDR_DHCP_CLIENT;
    struct {
        char ssid[33];
        char password[65];
    } legacy = {0};
    size_t stored = 0;
    const size_t count = lfs_kv_get_blob("WEBRTC_WIFI_CREDENTIALS", &legacy, sizeof(legacy), &stored);
    if (count == sizeof(legacy) && stored == count) {
        memcpy(saved_wifi.ssid, legacy.ssid, sizeof(saved_wifi.ssid));
        memcpy(saved_wifi.password, legacy.password, sizeof(saved_wifi.password));
    }
    clear_credentials(&legacy, sizeof(legacy));
    if (credentials_valid(&saved_wifi)) return;
    if (!read_config_string("WEBRTC_WIFI_SSID", saved_wifi.ssid, sizeof(saved_wifi.ssid)) ||
        !read_config_string("WEBRTC_WIFI_PASSWORD", saved_wifi.password, sizeof(saved_wifi.password)) ||
        !credentials_valid(&saved_wifi))
        clear_credentials(&saved_wifi, sizeof(saved_wifi));
}

static void save_wifi_credentials(bool associated) {
    struct net_al_ext_ip_addr_cfg config = {0};
    if (net_al_ext_get_vif_ip(MGMR_VIF_STA, &config) || !config.ipv4.addr ||
        (config.mode != IP_ADDR_DHCP_CLIENT && config.mode != IP_ADDR_STATIC_IPV4))
        return;
    wifi_static_ipv4_t address = {0};
    if (config.mode == IP_ADDR_STATIC_IPV4) {
        if (!config.ipv4.mask) return;
        address = (wifi_static_ipv4_t){config.ipv4.addr, config.ipv4.mask, config.ipv4.gw, config.ipv4.dns};
    }
    const bool new_address = config.mode != saved_wifi.ip_mode || memcmp(&address, &saved_ipv4, sizeof(address));
    if (!associated && profile_exists && !new_address) return;

    wifi_mgmr_connect_ind_stat_info_t info = {0};
    wifi_profile_t credentials = {.version = WIFI_PROFILE_VERSION, .ip_mode = config.mode};
    if (wifi_mgmr_sta_connect_ind_stat_get(&info) == 0) {
        memcpy(credentials.ssid, info.ssid, sizeof(credentials.ssid));
        memcpy(credentials.password, info.passphr, sizeof(credentials.password));
        memcpy(credentials.bssid, info.bssid, sizeof(credentials.bssid));
        if (credentials_valid(&credentials) && unicast_bssid(credentials.bssid) &&
            (!profile_exists || new_address || strcmp(credentials.ssid, saved_wifi.ssid) ||
             strcmp(credentials.password, saved_wifi.password) ||
             memcmp(credentials.bssid, saved_wifi.bssid, sizeof(credentials.bssid)))) {
            if (write_profile(&credentials, &address)) {
                saved_wifi = credentials;
                saved_ipv4 = address;
                profile_exists = true;
                printf("MicroPixel Wi-Fi profile saved (%s)\n", config.mode == IP_ADDR_STATIC_IPV4 ? "static" : "DHCP");
            } else {
                puts("MicroPixel Wi-Fi profile save failed");
            }
        }
    }
    clear_credentials(&credentials, sizeof(credentials));
    clear_credentials(&info, sizeof(info));
}

static int mpx_wifi_join(const mpx_wifi_network_t* network, const char* password, size_t length,
                         const wifi_profile_t* profile);

static void restore_wifi_connection(void) {
    if (!saved_wifi.ssid[0]) return;
    const size_t password_length = strlen(saved_wifi.password);
    mpx_wifi_network_t network = {.length = strlen(saved_wifi.ssid), .open = !password_length, .supported = true};
    memcpy(network.ssid, saved_wifi.ssid, network.length);
    if (mpx_wifi_join(&network, saved_wifi.password, password_length, &saved_wifi) == 0)
        puts("MicroPixel Wi-Fi reconnecting to saved profile");
}

static void restore_static_ipv4(void) {
    if (wifi_mgmr_sta_ip_set(saved_ipv4.addr, saved_ipv4.mask, saved_ipv4.gateway, saved_ipv4.dns) == 0) return;
    puts("MicroPixel Wi-Fi static IPv4 restore failed");
    (void)wifi_sta_disconnect();
}
#endif
#if defined(CONFIG_BLUETOOTH)
static const char kBleName[] = "MicroPixel Orion";
static bool start_ble;
static uint32_t ble_scan_end;
static struct bt_conn* ble_link;
#endif

static struct {
    uint32_t method;
    uint32_t index;
    uint8_t length;
    char password[64];
} command;

static void execute_command(uint32_t now_ms);

#if defined(CONFIG_FHOST)
static void wifi_event(async_input_event_t event, void* unused) {
    (void)unused;
    uint32_t ip = 0;
    if (event->code == CODE_WIFI_ON_GOT_IP) wifi_sta_ip4_addr_get(&ip, NULL, NULL, NULL);
    taskENTER_CRITICAL();
    switch (event->code) {
        case CODE_WIFI_ON_MGMR_DONE:
            status.wifi = MPX_WIFI_IDLE;
            wifi_restore_pending = true;
#if defined(CONFIG_BLUETOOTH)
            start_ble = true;
#endif
            break;
        case CODE_WIFI_ON_CONNECTING:
            status.wifi = MPX_WIFI_JOINING;
            break;
        case CODE_WIFI_ON_CONNECTED:
            status.wifi = MPX_WIFI_DHCP;
            wifi_apply_static = wifi_restore_static;
            wifi_restore_static = false;
            break;
        case CODE_WIFI_ON_GOT_IP:
            status.wifi = MPX_WIFI_ONLINE;
            status.ipv4 = ip;
            status.wifi_error = 0;
            wifi_join_pending = false;
            wifi_save_pending = true;
            break;
        case CODE_WIFI_ON_LOST_IP:
            status.wifi = MPX_WIFI_DHCP;
            status.ipv4 = 0;
            break;
        case CODE_WIFI_ON_GOT_IP_TIMEOUT:
        case CODE_WIFI_ON_PARAMS_ERROR:
        case CODE_WIFI_ON_DISCONNECT:
            status.wifi = wifi_join_pending ? MPX_WIFI_FAILED : MPX_WIFI_IDLE;
            status.wifi_error = wifi_join_pending ? (int)event->code : 0;
            status.ipv4 = 0;
            wifi_join_pending = false;
            wifi_restore_static = false;
            wifi_apply_static = false;
            break;
        case CODE_WIFI_ON_SCAN_DONE:
            wifi_scan_done = status.wifi_scanning;
            break;
        default:
            taskEXIT_CRITICAL();
            return;
    }
    ++status.revision;
    taskEXIT_CRITICAL();
    if (event->code == CODE_WIFI_ON_MGMR_DONE) puts("MicroPixel Wi-Fi ready");
    if (event->code == CODE_WIFI_ON_GOT_IP) puts("MicroPixel Wi-Fi connected with IPv4");
}

static void collect_wifi_results(void) {
    unsigned count = wifi_mgmr_sta_scanlist_dump(wifi_scan_cache, sizeof(wifi_scan_cache) / sizeof(wifi_scan_cache[0]));
    unsigned published = 0;
    taskENTER_CRITICAL();
    for (unsigned i = 0; i < count && published < MPX_WIFI_RESULTS; ++i) {
        const wifi_mgmr_scan_item_t* item = &wifi_scan_cache[i];
        if (item->ssid_len <= 0 || item->ssid_len > 32 || memchr(item->ssid, 0, item->ssid_len)) continue;
        unsigned index = 0;
        while (index < published && (status.networks[index].length != item->ssid_len ||
                                     memcmp(status.networks[index].ssid, item->ssid, item->ssid_len))) {
            ++index;
        }
        mpx_wifi_network_t* network = &status.networks[index];
        if (index < published && network->rssi >= item->rssi) continue;
        if (index == published) ++published;
        memcpy(network->ssid, item->ssid, item->ssid_len);
        network->ssid[item->ssid_len] = 0;
        network->length = item->ssid_len;
        network->rssi = item->rssi;
        network->open = item->auth == WIFI_EVENT_BEACON_IND_AUTH_OPEN;
        network->supported = network->open ||
                             (item->auth >= WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK &&
                              item->auth <= WIFI_EVENT_BEACON_IND_AUTH_WPA_WPA2_PSK) ||
                             item->auth == WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK_WPA3_SAE ||
                             item->auth == WIFI_EVENT_BEACON_IND_AUTH_WPA3_SAE;
    }
    status.wifi_count = published;
    status.wifi_scanning = false;
    ++status.revision;
    taskEXIT_CRITICAL();
    printf("MicroPixel Wi-Fi scan complete, %u networks\n", published);
}

#endif

#if defined(CONFIG_BLUETOOTH)
static void ble_connected(struct bt_conn* conn, uint8_t error) {
    struct bt_conn_info info;
    if (!error && (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE)) return;
    taskENTER_CRITICAL();
    if (error) {
        status.ble = MPX_BLE_IDLE;
    } else {
        ble_link = bt_conn_ref(conn);
        status.ble = info.role == BT_CONN_ROLE_SLAVE ? MPX_BLE_INCOMING : MPX_BLE_OUTGOING;
    }
    status.ble_error = error;
    ++status.revision;
    taskEXIT_CRITICAL();
    if (!error) {
        puts(info.role == BT_CONN_ROLE_SLAVE ? "MicroPixel BLE incoming connection"
                                             : "MicroPixel BLE outgoing connection");
    }
}

static void ble_disconnected(struct bt_conn* conn, uint8_t reason) {
    (void)reason;
    taskENTER_CRITICAL();
    const bool tracked = ble_link == conn;
    if (tracked) {
        ble_link = NULL;
        status.ble = MPX_BLE_IDLE;
        ++status.revision;
    }
    taskEXIT_CRITICAL();
    if (tracked) {
        bt_conn_unref(conn);
        puts("MicroPixel BLE disconnected");
    }
}

static struct bt_conn_cb ble_callbacks = {.connected = ble_connected, .disconnected = ble_disconnected};

static void ble_ready(int error) {
    if (!error) bt_conn_cb_register(&ble_callbacks);
    taskENTER_CRITICAL();
    status.ble = error ? MPX_BLE_UNAVAILABLE : MPX_BLE_IDLE;
    status.ble_error = error;
    ++status.revision;
    taskEXIT_CRITICAL();
    if (error)
        printf("MicroPixel BLE initialization failed (%d)\n", error);
    else
        puts("MicroPixel BLE ready");
}

static bool ble_parse_name(struct bt_data* data, void* context) {
    if (data->type != BT_DATA_NAME_COMPLETE && data->type != BT_DATA_NAME_SHORTENED) return true;
    char* name = context;
    size_t length = data->data_len < 32 ? data->data_len : 32;
    memcpy(name, data->data, length);
    name[length] = 0;
    return false;
}

static void ble_result(const bt_addr_le_t* address, int8_t rssi, uint8_t type, struct net_buf_simple* data) {
    const bool connectable = type == BT_LE_ADV_IND || type == BT_LE_ADV_DIRECT_IND;
    const bool response = type == BT_LE_ADV_SCAN_RSP;
    if (!connectable && !response) return;
    char name[33] = {0};
    bt_data_parse(data, ble_parse_name, name);
    taskENTER_CRITICAL();
    if (status.ble == MPX_BLE_SCANNING) {
        unsigned index = 0;
        while (index < status.ble_count && (status.devices[index].address_type != address->type ||
                                            memcmp(status.devices[index].mac, address->a.val, 6)))
            ++index;
        if (connectable && index == status.ble_count && index < MPX_BLE_RESULTS) {
            mpx_ble_device_t* device = &status.devices[index];
            memset(device, 0, sizeof(*device));
            bt_addr_to_str(&address->a, device->address, sizeof(device->address));
            memcpy(device->mac, address->a.val, 6);
            device->address_type = address->type;
            device->rssi = rssi;
            ++status.ble_count;
            ++status.revision;
        }
        if (index < status.ble_count && name[0] && strcmp(status.devices[index].name, name)) {
            memcpy(status.devices[index].name, name, sizeof(name));
            ++status.revision;
        }
    }
    taskEXIT_CRITICAL();
}

#endif

void mpx_wireless_init(void) {
#if defined(CONFIG_FHOST)
    load_wifi_credentials();
    const int result = async_register_event_filter(EV_WIFI, wifi_event, NULL);
    if (result != 0) {
        status.wifi = MPX_WIFI_UNAVAILABLE;
        printf("MicroPixel Wi-Fi event registration failed (%d)\n", result);
    }
#else
    status.wifi = MPX_WIFI_UNAVAILABLE;
#endif
#if defined(CONFIG_COEX)
    const int radio = wifi_mgmr_coex_rf_status_get();
    if (radio != WIFI_MGMR_COEX_OK) {
        status.wifi = MPX_WIFI_UNAVAILABLE;
        status.ble = MPX_BLE_UNAVAILABLE;
        printf("MicroPixel radio startup failed (%d)\n", radio);
    }
#elif defined(CONFIG_BLUETOOTH)
    start_ble = true;
#else
    status.ble = MPX_BLE_UNAVAILABLE;
#endif
}

// Keep flash and Wi-Fi work off the Orion display loop's stack frame under LTO.
__attribute__((noinline)) void mpx_wireless_poll(uint32_t now_ms) {
    taskENTER_CRITICAL();
    const bool command_pending = command.method != 0;
#if defined(CONFIG_FHOST)
    const bool scanned = wifi_scan_done;
    wifi_scan_done = false;
    const bool restore = wifi_restore_pending;
    wifi_restore_pending = false;
    const bool save = wifi_save_pending && status.wifi == MPX_WIFI_ONLINE;
    wifi_save_pending = false;
    const bool apply_static = wifi_apply_static;
    wifi_apply_static = false;
#endif
#if defined(CONFIG_BLUETOOTH)
    const bool initialize = start_ble;
    start_ble = false;
    const bool stop_scan = status.ble == MPX_BLE_SCANNING && (int32_t)(now_ms - ble_scan_end) >= 0;
#endif
    taskEXIT_CRITICAL();
#if defined(CONFIG_BLUETOOTH)
    if (initialize) {
        btble_controller_init(configMAX_PRIORITIES - 1);
        int error = hci_driver_init();
        if (!error) error = bt_enable(ble_ready);
        if (error) ble_ready(error);
    }
    if (stop_scan) {
        const int error = bt_le_scan_stop();
        taskENTER_CRITICAL();
        status.ble = MPX_BLE_IDLE;
        status.ble_error = error;
        ++status.revision;
        const unsigned found = status.ble_count;
        taskEXIT_CRITICAL();
        printf("MicroPixel BLE scan complete, %u connectable devices\n", found);
    }
#endif
#if defined(CONFIG_FHOST)
    if (scanned) collect_wifi_results();
    if (apply_static) restore_static_ipv4();
    if (restore && !command_pending) restore_wifi_connection();
    if (mpx_wifi_online()) {
        if (save || (int32_t)(now_ms - next_profile_check_ms) >= 0) {
            save_wifi_credentials(save);
            next_profile_check_ms = now_ms + 5000;
        }
        mpx_network_time_start();
    }
#endif
    if (command_pending) execute_command(now_ms);
}

bool mpx_wireless_read(mpx_wireless_status_t* destination, uint32_t previous_revision) {
    taskENTER_CRITICAL();
    const bool changed = status.revision != previous_revision;
    if (changed) memcpy(destination, &status, sizeof(status));
    taskEXIT_CRITICAL();
    return changed;
}

bool mpx_wifi_online(void) {
    taskENTER_CRITICAL();
    const bool online = status.wifi == MPX_WIFI_ONLINE && status.ipv4 != 0;
    taskEXIT_CRITICAL();
    return online;
}

#if defined(CONFIG_FHOST)
static int mpx_wifi_scan(void) {
    taskENTER_CRITICAL();
    const bool allowed = !status.wifi_scanning && (status.wifi == MPX_WIFI_IDLE || status.wifi == MPX_WIFI_ONLINE ||
                                                   status.wifi == MPX_WIFI_FAILED);
    if (allowed) {
        status.wifi_scanning = true;
        status.wifi_count = 0;
        status.wifi_error = 0;
        ++status.revision;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    wifi_mgmr_scan_params_t params = {0};
    const int error = wifi_mgmr_sta_scan(&params);
    if (error) {
        taskENTER_CRITICAL();
        status.wifi_scanning = false;
        status.wifi_error = error;
        ++status.revision;
        taskEXIT_CRITICAL();
    }
    return error;
}

static int mpx_wifi_join(const mpx_wifi_network_t* network, const char* password, size_t length,
                         const wifi_profile_t* profile) {
    if (!network->supported || network->length == 0 || network->length > 32 ||
        (network->open ? length != 0 : length < 8 || length > 63))
        return -EINVAL;
    const bool restore_static = profile && profile->ip_mode == IP_ADDR_STATIC_IPV4;
    taskENTER_CRITICAL();
    const bool allowed = !status.wifi_scanning && (status.wifi == MPX_WIFI_IDLE || status.wifi == MPX_WIFI_FAILED);
    if (allowed) {
        wifi_join_pending = true;
        wifi_restore_static = restore_static;
        status.wifi = MPX_WIFI_JOINING;
        status.wifi_error = 0;
        ++status.revision;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    wifi_mgmr_sta_connect_params_t params = {0};
    memcpy(params.ssid, network->ssid, network->length);
    params.ssid_len = network->length;
    if (length) memcpy(params.key, password, length);
    params.key_len = length;
    if (profile && unicast_bssid(profile->bssid)) {
        const uint8_t* bssid = profile->bssid;
        snprintf(params.bssid_str, sizeof(params.bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1],
                 bssid[2], bssid[3], bssid[4], bssid[5]);
    }
    params.use_dhcp = !restore_static;
    params.dhcp_timeout_event_only = 1;
    const int error = wifi_mgmr_sta_connect(&params);
    for (volatile char* p = params.key; p < params.key + sizeof(params.key); ++p) *p = 0;
    if (error) {
        taskENTER_CRITICAL();
        wifi_join_pending = false;
        wifi_restore_static = false;
        status.wifi = MPX_WIFI_FAILED;
        status.wifi_error = error;
        ++status.revision;
        taskEXIT_CRITICAL();
    }
    return error;
}

static int mpx_wifi_leave(void) {
    taskENTER_CRITICAL();
    const bool active = status.wifi == MPX_WIFI_ONLINE || status.wifi == MPX_WIFI_JOINING ||
                        status.wifi == MPX_WIFI_DHCP || status.wifi == MPX_WIFI_FAILED;
    wifi_join_pending = false;
    wifi_restore_static = false;
    wifi_apply_static = false;
    taskEXIT_CRITICAL();
    return active ? wifi_sta_disconnect() : -EINVAL;
}

#endif

#if defined(CONFIG_BLUETOOTH)
static int ble_command_result(int error, uint8_t state_on_error) {
    taskENTER_CRITICAL();
    if (error) status.ble = state_on_error;
    status.ble_error = error;
    ++status.revision;
    taskEXIT_CRITICAL();
    return error;
}

static int mpx_ble_scan(uint32_t now_ms) {
    taskENTER_CRITICAL();
    const bool allowed = status.ble == MPX_BLE_IDLE;
    if (allowed) {
        status.ble = MPX_BLE_SCANNING;
        status.ble_count = 0;
        ble_scan_end = now_ms + 6000;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    const struct bt_le_scan_param params = {
        .type = BT_LE_SCAN_TYPE_ACTIVE, .interval = BT_GAP_SCAN_FAST_INTERVAL, .window = BT_GAP_SCAN_FAST_WINDOW};
    return ble_command_result(bt_le_scan_start(&params, ble_result), MPX_BLE_IDLE);
}

static int mpx_ble_advertise(bool enabled) {
    taskENTER_CRITICAL();
    const uint8_t previous = status.ble;
    const bool allowed = previous == (enabled ? MPX_BLE_IDLE : MPX_BLE_ADVERTISING);
    if (allowed) status.ble = enabled ? MPX_BLE_ADVERTISING : MPX_BLE_IDLE;
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
    const struct bt_data ad[] = {
        {.type = BT_DATA_FLAGS, .data_len = sizeof(flags), .data = &flags},
        {.type = BT_DATA_NAME_COMPLETE, .data_len = sizeof(kBleName) - 1, .data = (const uint8_t*)kBleName}};
    const struct bt_le_adv_param params = {.options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME,
                                           .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
                                           .interval_max = BT_GAP_ADV_FAST_INT_MAX_2};
    const int error = enabled ? bt_le_adv_start(&params, ad, sizeof(ad) / sizeof(ad[0]), NULL, 0) : bt_le_adv_stop();
    if (!error && enabled) puts("MicroPixel BLE connectable advertising started");
    return ble_command_result(error, previous);
}

static int mpx_ble_connect(const mpx_ble_device_t* device) {
    taskENTER_CRITICAL();
    const bool allowed = status.ble == MPX_BLE_IDLE;
    if (allowed) status.ble = MPX_BLE_CONNECTING;
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    bt_addr_le_t peer = {.type = device->address_type};
    memcpy(peer.a.val, device->mac, sizeof(peer.a.val));
    struct bt_conn* conn = bt_conn_create_le(&peer, BT_LE_CONN_PARAM_DEFAULT);
    if (conn == NULL) return ble_command_result(-EIO, MPX_BLE_IDLE);
    bt_conn_unref(conn);
    taskENTER_CRITICAL();
    ++status.revision;
    taskEXIT_CRITICAL();
    return 0;
}

static int mpx_ble_disconnect(void) {
    taskENTER_CRITICAL();
    struct bt_conn* conn = ble_link ? bt_conn_ref(ble_link) : NULL;
    taskEXIT_CRITICAL();
    if (conn == NULL) return -ENOTCONN;
    const int error = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    bt_conn_unref(conn);
    return error;
}

#endif

int mpx_wireless_command(uint32_t method, uint32_t index, const char* password, uint8_t length) {
    if (method < MICROPIXEL_WIRELESS_WIFI_SCAN || method > MICROPIXEL_WIRELESS_BLE_DISCONNECT ||
        length > sizeof(command.password) || (length && password == NULL))
        return -EINVAL;
    taskENTER_CRITICAL();
    const bool accepted = command.method == 0;
    if (accepted) {
        command.method = method;
        command.index = index;
        command.length = length;
        if (length) memcpy(command.password, password, length);
    }
    taskEXIT_CRITICAL();
    return accepted ? 0 : -EBUSY;
}

static void execute_command(uint32_t now_ms) {
    // The pending mailbox is immutable until its sole consumer clears it.
    const uint32_t method = command.method;
    const uint32_t index = command.index;
    int error;
    (void)now_ms;
    switch (method) {
#if defined(CONFIG_FHOST)
        case MICROPIXEL_WIRELESS_WIFI_SCAN:
            error = mpx_wifi_scan();
            break;
        case MICROPIXEL_WIRELESS_WIFI_JOIN:
            error = index < status.wifi_count
                        ? mpx_wifi_join(&status.networks[index], command.password, command.length, NULL)
                        : -EINVAL;
            break;
        case MICROPIXEL_WIRELESS_WIFI_LEAVE:
            error = mpx_wifi_leave();
            break;
#endif
#if defined(CONFIG_BLUETOOTH)
        case MICROPIXEL_WIRELESS_BLE_SCAN:
            error = mpx_ble_scan(now_ms);
            break;
        case MICROPIXEL_WIRELESS_BLE_ADVERTISE:
            error = mpx_ble_advertise(true);
            break;
        case MICROPIXEL_WIRELESS_BLE_HIDE:
            error = mpx_ble_advertise(false);
            break;
        case MICROPIXEL_WIRELESS_BLE_CONNECT: {
            mpx_ble_device_t device;
            taskENTER_CRITICAL();
            const bool valid = status.ble == MPX_BLE_IDLE && index < status.ble_count;
            if (valid) device = status.devices[index];
            taskEXIT_CRITICAL();
            error = valid ? mpx_ble_connect(&device) : -EINVAL;
            break;
        }
        case MICROPIXEL_WIRELESS_BLE_DISCONNECT:
            error = mpx_ble_disconnect();
            break;
#endif
        default:
            error = -ENOTSUP;
            break;
    }
    for (volatile char* p = command.password; p < command.password + sizeof(command.password); ++p) *p = 0;
    taskENTER_CRITICAL();
    if (error) {
        if (method <= MICROPIXEL_WIRELESS_WIFI_LEAVE)
            status.wifi_error = error;
        else
            status.ble_error = error;
        ++status.revision;
    }
    command.method = 0;
    taskEXIT_CRITICAL();
}
