// SPDX-License-Identifier: Apache-2.0
#include "wireless.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#if defined(CONFIG_FHOST)
#include "async_event.h"
#include "bflb_core.h"
#include "lfs_kv.h"
#include "net_al.h"
#include "net_al_ext.h"
#include "platform/network/network_time.h"
#include "platform/storage/file_store.h"
#include "wifi_history.h"
#include "wifi_mgmr_ext.h"
#include "wifi_scan_results.h"
#endif
#if defined(CONFIG_COEX)
#include "wifi_mgmr_coex.h"
#endif
#if defined(CONFIG_BT_A2DP)
#include "classic_bluetooth.h"
#endif
#if defined(CONFIG_BLUETOOTH)
#include "bluetooth.h"
#include "bt_uuid.h"
#include "btble_lib_api.h"
#include "buf.h"
#include "conn.h"
#include "gatt.h"
#include "hci_driver.h"
#include "hci_err.h"
#endif
#if defined(MICROPIXEL_ORION_BLE_SMOKE)
#include "diagnostics/ble_smoke.h"
#endif
#if defined(MICROPIXEL_ORION_BLE_PROFILES)
#include "diagnostics/ble_profiles.h"
#endif

static mpx_wireless_status_t status = {.revision = 1};

#if defined(CONFIG_FHOST)
enum {
    WIFI_SCAN_RETRY_MS = 120000,
    WIFI_MANUAL_GRACE_MS = 60000,
    WIFI_ADDRESS_TIMEOUT_MS = WIFI_STA_DHCPC_TIMEOUT_MS_DEFAULT + 5000,
    WIFI_PROFILE_CHECK_MS = 5000,
};

typedef enum { WIFI_SCAN_NONE, WIFI_SCAN_MANUAL, WIFI_SCAN_BACKGROUND } wifi_scan_kind_t;
static ATTR_NOINIT_PSRAM_SECTION wifi_mgmr_scan_item_t wifi_scan_cache[MPX_WIFI_RESULTS];
static ATTR_NOINIT_PSRAM_SECTION mpx_wifi_network_t wifi_networks[MPX_WIFI_RESULTS];
static ATTR_NOINIT_PSRAM_SECTION mpx_wifi_network_t wifi_background_networks[MPX_WIFI_RESULTS];
static wifi_scan_kind_t wifi_scan_kind;
static bool wifi_scan_done;
static bool wifi_manager_ready;
static bool wifi_join_pending;
static bool wifi_save_pending;
static bool wifi_connected_profile_ready;
static mpx_wifi_profile_t wifi_connected_profile;
static bool wifi_restore_static;
static bool wifi_apply_static;
static mpx_wifi_ipv4_t wifi_restore_ipv4;
static bool wifi_ip_timeout;
static bool wifi_disconnect_pending;
static int wifi_disconnect_error;
static struct {
    uint8_t index;
    uint8_t length;
    bool ready;
    char password[64];
} wifi_fallback = {.index = MPX_WIFI_NO_INDEX};

// Controller-owned state. Scan results stay valid until the next background scan.
static unsigned wifi_background_count;
static size_t wifi_next_profile;
static bool wifi_scan_waiting;
static uint32_t wifi_next_scan_ms;
static bool wifi_address_waiting;
static uint32_t wifi_address_deadline_ms;
static uint32_t next_profile_check_ms;
static mpx_wifi_profile_t wifi_timebox_profile;

// These helpers run with the status critical section held.
static void clear_wifi_fallback(void) {
    mpx_wifi_clear_credentials(wifi_fallback.password, sizeof(wifi_fallback.password));
    wifi_fallback.index = MPX_WIFI_NO_INDEX;
    wifi_fallback.length = 0;
    wifi_fallback.ready = false;
}

static bool schedule_wifi_fallback(void) {
    if (status.wifi != MPX_WIFI_JOINING || wifi_fallback.index == MPX_WIFI_NO_INDEX) return false;
    wifi_fallback.ready = true;
    return true;
}

static bool read_config_string(const char* key, char* value, size_t capacity) {
    size_t stored = 0;
    const size_t count = lfs_kv_get_blob(key, value, capacity, &stored);
    return count && count == stored && memchr(value, 0, count) != NULL;
}

static bool migrate_single_profile(void) {
    struct {
        uint8_t version;
        uint8_t ip_mode;
        uint8_t bssid[6];
        char ssid[33];
        char password[65];
    } legacy = {0};
    _Static_assert(sizeof(legacy) == 106, "Version 1 Wi-Fi profile size");
    mpx_wifi_profile_t profile = {0};
    uint8_t data[sizeof(legacy) + sizeof(profile.ipv4)];
    size_t size = 0;
    bool migrated = false;
    if (!mpx_file_read("settings.wifi", data, sizeof(data), &size) && size >= sizeof(legacy)) {
        memcpy(&legacy, data, sizeof(legacy));
        memcpy(profile.ssid, legacy.ssid, sizeof(profile.ssid));
        memcpy(profile.password, legacy.password, sizeof(profile.password));
        memcpy(profile.bssid, legacy.bssid, sizeof(profile.bssid));
        const bool dhcp = legacy.ip_mode == IP_ADDR_DHCP_CLIENT && size == sizeof(legacy);
        const bool fixed = legacy.ip_mode == IP_ADDR_STATIC_IPV4 && size == sizeof(data);
        if (fixed) memcpy(&profile.ipv4, data + sizeof(legacy), sizeof(profile.ipv4));
        if (legacy.version == 1 && (dhcp || (fixed && profile.ipv4.addr)))
            migrated = mpx_wifi_history_remember(&profile);
    }
    mpx_wifi_clear_credentials(&profile, sizeof(profile));
    mpx_wifi_clear_credentials(&legacy, sizeof(legacy));
    mpx_wifi_clear_credentials(data, sizeof(data));
    return migrated;
}

static void load_wifi_credentials(void) {
    if (!mpx_files_mount()) puts("MicroPixel Wi-Fi profile filesystem unavailable");
    if (mpx_wifi_history_load()) return;
    if (migrate_single_profile()) {
        mpx_wifi_history_flush();
        return;
    }

    // Timebox credentials have no BSSID. Bind them after a successful connection.
    struct {
        char ssid[33];
        char password[65];
    } legacy = {0};
    size_t stored = 0;
    const size_t count = lfs_kv_get_blob("WEBRTC_WIFI_CREDENTIALS", &legacy, sizeof(legacy), &stored);
    if (count == sizeof(legacy) && stored == count) {
        memcpy(wifi_timebox_profile.ssid, legacy.ssid, sizeof(legacy.ssid));
        memcpy(wifi_timebox_profile.password, legacy.password, sizeof(legacy.password));
    }
    mpx_wifi_clear_credentials(&legacy, sizeof(legacy));
    if (mpx_wifi_profile_credentials_valid(&wifi_timebox_profile)) return;

    mpx_wifi_clear_credentials(&wifi_timebox_profile, sizeof(wifi_timebox_profile));
    if (!read_config_string("WEBRTC_WIFI_SSID", wifi_timebox_profile.ssid, sizeof(wifi_timebox_profile.ssid)) ||
        !read_config_string("WEBRTC_WIFI_PASSWORD", wifi_timebox_profile.password,
                            sizeof(wifi_timebox_profile.password)) ||
        !mpx_wifi_profile_credentials_valid(&wifi_timebox_profile))
        mpx_wifi_clear_credentials(&wifi_timebox_profile, sizeof(wifi_timebox_profile));
}

static bool read_wifi_profile(mpx_wifi_profile_t* profile) {
    struct net_al_ext_ip_addr_cfg config = {0};
    if (net_al_ext_get_vif_ip(MGMR_VIF_STA, &config) || !config.ipv4.addr ||
        (config.mode != IP_ADDR_DHCP_CLIENT && config.mode != IP_ADDR_STATIC_IPV4) ||
        (config.mode == IP_ADDR_STATIC_IPV4 && !config.ipv4.mask))
        return false;
    wifi_mgmr_connect_ind_stat_info_t info = {0};
    const int result = wifi_mgmr_sta_connect_ind_stat_get(&info);
    const char* ssid_end = memchr(info.ssid, 0, sizeof(info.ssid));
    const char* password_end = memchr(info.passphr, 0, sizeof(info.passphr));
    const bool read = result == 0 && ssid_end && password_end;
    if (read) {
        *profile = (mpx_wifi_profile_t){.channel = info.channel};
        if (config.mode == IP_ADDR_STATIC_IPV4)
            profile->ipv4 = (mpx_wifi_ipv4_t){config.ipv4.addr, config.ipv4.mask, config.ipv4.gw, config.ipv4.dns};
        memcpy(profile->ssid, info.ssid, ssid_end - info.ssid);
        memcpy(profile->password, info.passphr, password_end - info.passphr);
        memcpy(profile->bssid, info.bssid, sizeof(profile->bssid));
    }
    mpx_wifi_clear_credentials(&info, sizeof(info));
    return read;
}

static void remember_wifi_profile(const mpx_wifi_profile_t* profile) {
    if (mpx_wifi_history_remember(profile))
        mpx_wifi_clear_credentials(&wifi_timebox_profile, sizeof(wifi_timebox_profile));
    else
        puts("MicroPixel Wi-Fi profile could not be recorded");
}

static void poll_wifi_reconnect(uint32_t now_ms);
static void poll_wifi_ip(uint32_t now_ms);
static void retry_wifi_connection(void);

static void finish_wifi_disconnect(int error) {
    status.wifi = wifi_join_pending ? MPX_WIFI_FAILED : MPX_WIFI_IDLE;
    status.wifi_error = wifi_join_pending ? error : 0;
    status.ipv4 = 0;
    wifi_join_pending = false;
    wifi_disconnect_pending = false;
    wifi_restore_static = false;
    wifi_apply_static = false;
    wifi_ip_timeout = false;
    clear_wifi_fallback();
}
#endif
#if defined(CONFIG_BLUETOOTH)
static bool start_ble;
static bool ble_resume_advertising;
static uint32_t ble_scan_end;
static struct bt_conn* ble_link;
static struct bt_conn* ble_rejected_link;
static int mpx_ble_stop_scan(void);
static int mpx_ble_start_advertising(void);
#endif

static struct {
    uint32_t method;
    uint32_t index;
    uint32_t scan_revision;
    uint8_t length;
    char password[64];
} command;

static void execute_command(uint32_t now_ms);

#if defined(CONFIG_FHOST)
static void wifi_event(async_input_event_t event, void* unused) {
    (void)unused;
    uint32_t ip = 0;
    mpx_wifi_profile_t profile;
    bool profile_ready = false;
    if (event->code == CODE_WIFI_ON_GOT_IP) {
        wifi_sta_ip4_addr_get(&ip, NULL, NULL, NULL);
        if (ip) profile_ready = read_wifi_profile(&profile);
    }
    taskENTER_CRITICAL();
    switch (event->code) {
        case CODE_WIFI_ON_MGMR_DONE:
            status.wifi = MPX_WIFI_IDLE;
            wifi_manager_ready = true;
#if defined(CONFIG_BLUETOOTH)
            start_ble = true;
#endif
            break;
        case CODE_WIFI_ON_CONNECTING:
            if (status.wifi != MPX_WIFI_DHCP && status.wifi != MPX_WIFI_ONLINE) status.wifi = MPX_WIFI_JOINING;
            break;
        case CODE_WIFI_ON_CONNECTED:
            if (status.wifi == MPX_WIFI_ONLINE) break;
            status.wifi = MPX_WIFI_DHCP;
            wifi_apply_static = wifi_restore_static;
            wifi_restore_static = false;
            clear_wifi_fallback();
            break;
        case CODE_WIFI_ON_GOT_IP:
            if (!ip || wifi_disconnect_pending) break;
            status.wifi = MPX_WIFI_ONLINE;
            status.ipv4 = ip;
            status.wifi_error = 0;
            wifi_join_pending = false;
            wifi_save_pending = true;
            if (profile_ready) {
                wifi_connected_profile = profile;
                wifi_connected_profile_ready = true;
            }
            clear_wifi_fallback();
            break;
        case CODE_WIFI_ON_LOST_IP:
            if (status.wifi == MPX_WIFI_ONLINE) {
                status.wifi = MPX_WIFI_DHCP;
                status.ipv4 = 0;
            }
            break;
        case CODE_WIFI_ON_GOT_IP_TIMEOUT:
            if (status.wifi == MPX_WIFI_DHCP) wifi_ip_timeout = true;
            break;
        case CODE_WIFI_ON_PARAMS_ERROR:
        case CODE_WIFI_ON_DISCONNECT:
            if (!wifi_disconnect_pending && schedule_wifi_fallback()) break;
            finish_wifi_disconnect(wifi_disconnect_pending ? wifi_disconnect_error : (int)event->code);
            break;
        case CODE_WIFI_ON_SCAN_DONE:
            wifi_scan_done = wifi_scan_kind != WIFI_SCAN_NONE;
            break;
        default:
            taskEXIT_CRITICAL();
            return;
    }
    ++status.revision;
    taskEXIT_CRITICAL();
    if (profile_ready) mpx_wifi_clear_credentials(&profile, sizeof(profile));
    if (event->code == CODE_WIFI_ON_MGMR_DONE) puts("MicroPixel Wi-Fi ready");
    if (ip) puts("MicroPixel Wi-Fi connected with IPv4");
}

static void delay_wifi_scan(uint32_t now_ms, uint32_t delay_ms) {
    wifi_scan_waiting = true;
    wifi_next_scan_ms = now_ms + delay_ms;
}

static void collect_wifi_results(uint32_t now_ms) {
    const bool manual = wifi_scan_kind == WIFI_SCAN_MANUAL;
    mpx_wifi_network_t* networks = manual ? wifi_networks : wifi_background_networks;
    const unsigned count = wifi_mgmr_sta_scanlist_dump(wifi_scan_cache, MPX_WIFI_RESULTS);
    for (unsigned i = 0; i < count; ++i) {
        const wifi_mgmr_scan_item_t* item = &wifi_scan_cache[i];
        mpx_wifi_network_t* network = &networks[i];
        *network = (mpx_wifi_network_t){.rssi = item->rssi, .channel = item->channel};
        memcpy(network->bssid, item->bssid, sizeof(network->bssid));
        if (item->ssid_len <= 0 || item->ssid_len > 32 || memchr(item->ssid, 0, item->ssid_len)) continue;
        memcpy(network->ssid, item->ssid, item->ssid_len);
        network->length = item->ssid_len;
        network->open = item->auth == WIFI_EVENT_BEACON_IND_AUTH_OPEN;
        network->supported =
            mpx_wifi_unicast_bssid(network->bssid) && (network->open ||
                                                       (item->auth >= WIFI_EVENT_BEACON_IND_AUTH_WPA_PSK &&
                                                        item->auth <= WIFI_EVENT_BEACON_IND_AUTH_WPA_WPA2_PSK) ||
                                                       item->auth == WIFI_EVENT_BEACON_IND_AUTH_WPA2_PSK_WPA3_SAE ||
                                                       item->auth == WIFI_EVENT_BEACON_IND_AUTH_WPA3_SAE);
    }
    if (manual) {
        mpx_wifi_prepare_results(networks, count);
        delay_wifi_scan(now_ms, WIFI_MANUAL_GRACE_MS);
    } else {
        wifi_background_count = count;
        wifi_next_profile = 0;
        delay_wifi_scan(now_ms, WIFI_SCAN_RETRY_MS);
    }
    taskENTER_CRITICAL();
    wifi_scan_kind = WIFI_SCAN_NONE;
    if (manual) {
        status.wifi_count = count;
        status.wifi_scanning = false;
        ++status.revision;
    }
    taskEXIT_CRITICAL();
    if (manual) printf("MicroPixel Wi-Fi scan complete, %u networks\n", count);
}

#endif

#if defined(CONFIG_BLUETOOTH)
static ssize_t ble_read_model(struct bt_conn* conn, const struct bt_gatt_attr* attr, void* buffer, uint16_t length,
                              uint16_t offset) {
    static const char model[] = "BL618DG Orion";
    return bt_gatt_attr_read(conn, attr, buffer, length, offset, model, sizeof(model) - 1);
}

static struct bt_gatt_attr ble_device_attributes[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DIS),
    BT_GATT_CHARACTERISTIC(BT_UUID_DIS_MODEL_NUMBER, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, ble_read_model, NULL, NULL),
};
static struct bt_gatt_service ble_device_service = BT_GATT_SERVICE(ble_device_attributes);

static void ble_connected(struct bt_conn* conn, uint8_t error) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_LE) return;
    taskENTER_CRITICAL();
    const bool connecting = status.ble == MPX_BLE_CONNECTING;
    if (error) {
        const bool tracked = ble_link == conn;
        if (connecting && (!ble_link || tracked)) {
            const bool cancelled = status.ble_info.flags & MPX_BLE_DISCONNECT_PENDING;
            ble_link = NULL;
            status.ble = MPX_BLE_IDLE;
            status.ble_info.flags &= ~MPX_BLE_DISCONNECT_PENDING;
            status.ble_info.peer_name[0] = 0;
            status.ble_info.peer_address[0] = 0;
            const bool cancel_complete =
                error == BT_HCI_ERR_REMOTE_USER_TERM_CONN || error == BT_HCI_ERR_UNKNOWN_CONN_ID;
            status.ble_error = cancelled && cancel_complete ? 0 : error;
            ++status.revision;
        }
        taskEXIT_CRITICAL();
        if (connecting && tracked) bt_conn_unref(conn);
        return;
    }
    const bool incoming = info.role == BT_CONN_ROLE_SLAVE;
    const bool discoverable = status.ble_info.flags & MPX_BLE_DISCOVERY_ENABLED;
    const bool accept = incoming ? discoverable && !ble_link && status.ble == MPX_BLE_ADVERTISING
                                 : connecting && (!ble_link || ble_link == conn);
    if (accept) {
        if (!ble_link) ble_link = bt_conn_ref(conn);
        status.ble = incoming ? MPX_BLE_INCOMING : MPX_BLE_OUTGOING;
        if (incoming) {
            status.ble_info.peer_name[0] = 0;
            bt_addr_to_str(&info.le.dst->a, status.ble_info.peer_address, sizeof(status.ble_info.peer_address));
        }
        status.ble_error = 0;
        ++status.revision;
    } else if (!ble_rejected_link) {
        ble_rejected_link = bt_conn_ref(conn);
    }
    taskEXIT_CRITICAL();
    if (accept) puts(incoming ? "MicroPixel BLE incoming connection" : "MicroPixel BLE outgoing connection");
}

static void ble_disconnected(struct bt_conn* conn, uint8_t reason) {
    taskENTER_CRITICAL();
    const bool tracked = ble_link == conn;
    if (tracked) {
        ble_link = NULL;
        const bool user_disconnect = (status.ble_info.flags & MPX_BLE_DISCONNECT_PENDING) ||
                                     command.method == MICROPIXEL_WIRELESS_BLE_DISCONNECT;
        if (user_disconnect) status.ble_info.flags &= ~MPX_BLE_DISCOVERY_ENABLED;
        ble_resume_advertising = status.ble == MPX_BLE_INCOMING && (status.ble_info.flags & MPX_BLE_DISCOVERY_ENABLED);
        status.ble = ble_resume_advertising ? MPX_BLE_ADVERTISING : MPX_BLE_IDLE;
        status.ble_info.flags &= ~MPX_BLE_DISCONNECT_PENDING;
        status.ble_info.peer_name[0] = 0;
        status.ble_info.peer_address[0] = 0;
        ++status.revision;
    }
    taskEXIT_CRITICAL();
    if (tracked) {
        bt_conn_unref(conn);
        printf("MicroPixel BLE disconnected (reason=0x%02x)\n", (unsigned)reason);
    }
}

static struct bt_conn_cb ble_callbacks = {.connected = ble_connected, .disconnected = ble_disconnected};

static void ble_ready(int error) {
#if defined(CONFIG_BT_A2DP)
    if (!error) mpx_classic_ready();
#endif
    if (!error) error = bt_gatt_service_register(&ble_device_service);
#if defined(MICROPIXEL_ORION_BLE_SMOKE)
    if (!error) error = mpx_ble_smoke_init();
#endif
#if defined(MICROPIXEL_ORION_BLE_PROFILES)
    if (!error) error = mpx_ble_profiles_init();
#endif
    if (!error) bt_conn_cb_register(&ble_callbacks);
    taskENTER_CRITICAL();
    status.ble = error ? MPX_BLE_UNAVAILABLE : MPX_BLE_IDLE;
    status.ble_error = error;
    if (!error) memcpy(status.ble_info.local_name, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME));
    ++status.revision;
    taskEXIT_CRITICAL();
    if (error)
        printf("MicroPixel BLE initialization failed (%d)\n", error);
    else
        puts("MicroPixel BLE ready");
}

static void ble_result(const bt_addr_le_t* address, int8_t rssi, uint8_t type, struct net_buf_simple* data) {
    const bool connectable = type == BT_LE_ADV_IND || type == BT_LE_ADV_DIRECT_IND;
    const bool response = type == BT_LE_ADV_SCAN_RSP;
    if (!connectable && !response) return;
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
            ++status.ble_count;
        }
        if (index < status.ble_count) {
            mpx_ble_device_t* device = &status.devices[index];
            device->rssi = rssi;
            mpx_bt_scan_parse(&device->scan, data->data, data->len);
            device->group = mpx_bt_scan_le_group(&device->scan);
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
#if defined(CONFIG_BT_A2DP)
    mpx_classic_poll();
#endif
    taskENTER_CRITICAL();
    const bool command_pending = command.method != 0;
#if defined(CONFIG_FHOST)
    const bool scanned = wifi_scan_done;
    wifi_scan_done = false;
    const bool manager_ready = wifi_manager_ready;
    wifi_manager_ready = false;
    const bool save = wifi_save_pending;
    wifi_save_pending = false;
    const bool have_profile = wifi_connected_profile_ready;
    mpx_wifi_profile_t profile;
    if (have_profile) {
        profile = wifi_connected_profile;
        mpx_wifi_clear_credentials(&wifi_connected_profile, sizeof(wifi_connected_profile));
        wifi_connected_profile_ready = false;
    }
#endif
#if defined(CONFIG_BLUETOOTH)
    const bool initialize = start_ble;
    start_ble = false;
    const bool stop_scan = status.ble == MPX_BLE_SCANNING && (int32_t)(now_ms - ble_scan_end) >= 0;
    const bool resume_advertising = ble_resume_advertising && !command_pending;
    struct bt_conn* rejected = ble_rejected_link;
    ble_rejected_link = NULL;
#endif
    taskEXIT_CRITICAL();
#if defined(CONFIG_BLUETOOTH)
    if (initialize) {
        btble_controller_init(configMAX_PRIORITIES - 1);
        int error = hci_driver_init();
        if (!error) error = bt_enable(ble_ready);
        if (error) ble_ready(error);
    }
    if (rejected) {
        (void)bt_conn_disconnect(rejected, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(rejected);
    }
    if (stop_scan) {
        const int error = mpx_ble_stop_scan();
        if (error) {
            taskENTER_CRITICAL();
            status.ble_error = error;
            ble_scan_end = now_ms + 6000;
            ++status.revision;
            taskEXIT_CRITICAL();
        }
    }
    if (resume_advertising) {
        const int error = mpx_ble_start_advertising();
        if (error) {
            taskENTER_CRITICAL();
            status.ble_error = error;
            ++status.revision;
            taskEXIT_CRITICAL();
        }
    }
#endif
#if defined(CONFIG_FHOST)
    if (manager_ready && wifi_mgmr_sta_autoconnect_disable()) puts("MicroPixel Wi-Fi SDK auto-connect disable failed");
    if (scanned) collect_wifi_results(now_ms);
    if (save) {
        wifi_background_count = 0;
        wifi_scan_waiting = false;
    }
    const bool online = mpx_wifi_online();
    if (save || (int32_t)(now_ms - next_profile_check_ms) >= 0) {
        if (have_profile || (online && read_wifi_profile(&profile))) {
            remember_wifi_profile(&profile);
            mpx_wifi_clear_credentials(&profile, sizeof(profile));
        }
        mpx_wifi_history_flush();
        next_profile_check_ms = now_ms + WIFI_PROFILE_CHECK_MS;
    }
    if (online) mpx_network_time_start();
#endif
    if (command_pending) execute_command(now_ms);
#if defined(MICROPIXEL_ORION_BLE_SMOKE)
    mpx_ble_smoke_poll();
#endif
#if defined(MICROPIXEL_ORION_BLE_PROFILES)
    mpx_ble_profiles_poll(now_ms);
#endif
#if defined(CONFIG_FHOST)
    poll_wifi_ip(now_ms);
    retry_wifi_connection();
    poll_wifi_reconnect(now_ms);
#endif
}

bool mpx_wireless_read(mpx_wireless_status_t* destination, uint32_t previous_revision) {
    taskENTER_CRITICAL();
    const bool changed = status.revision != previous_revision;
    if (changed) memcpy(destination, &status, sizeof(status));
    taskEXIT_CRITICAL();
    return changed;
}

bool mpx_wireless_read_wifi(uint32_t revision, uint32_t index, mpx_wifi_network_t* network,
                            uint8_t* preferred_channel) {
#if defined(CONFIG_FHOST)
    taskENTER_CRITICAL();
    const bool valid = revision == status.wifi_scan_revision && index < status.wifi_count;
    if (valid) {
        *network = wifi_networks[index];
        *preferred_channel = wifi_networks[network->preferred].channel;
    }
    taskEXIT_CRITICAL();
    return valid;
#else
    (void)revision;
    (void)index;
    (void)network;
    (void)preferred_channel;
    return false;
#endif
}

bool mpx_wifi_online(void) {
    taskENTER_CRITICAL();
    const bool online = status.wifi == MPX_WIFI_ONLINE && status.ipv4 != 0;
    taskEXIT_CRITICAL();
    return online;
}

#if defined(CONFIG_FHOST)
static bool wifi_idle(void) { return status.wifi == MPX_WIFI_IDLE || status.wifi == MPX_WIFI_FAILED; }

static int mpx_wifi_scan(bool manual) {
    taskENTER_CRITICAL();
    const bool promote = manual && wifi_scan_kind == WIFI_SCAN_BACKGROUND;
    const bool allowed =
        promote || (wifi_scan_kind == WIFI_SCAN_NONE && (wifi_idle() || (manual && status.wifi == MPX_WIFI_ONLINE)) &&
                    (manual || !command.method));
    if (allowed) {
        wifi_scan_kind = manual ? WIFI_SCAN_MANUAL : WIFI_SCAN_BACKGROUND;
        if (manual) {
            status.wifi_scanning = true;
            status.wifi_count = 0;
            status.wifi_error = 0;
            ++status.wifi_scan_revision;
            ++status.revision;
        }
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    if (promote) return 0;

    wifi_mgmr_scan_params_t params = {0};
    const int error = wifi_mgmr_sta_scan(&params);
    if (error) {
        taskENTER_CRITICAL();
        wifi_scan_kind = WIFI_SCAN_NONE;
        wifi_scan_done = false;
        if (manual) {
            status.wifi_scanning = false;
            status.wifi_error = error;
            ++status.revision;
        }
        taskEXIT_CRITICAL();
    }
    return error;
}

static uint16_t wifi_channel_frequency(uint8_t channel) {
    if (channel >= 1 && channel <= 13) return 2407 + 5 * channel;
    if (channel == 14) return 2484;
    if (channel >= 32 && channel <= 177) return 5000 + 5 * channel;
    return 0;
}

static int start_wifi_connection(const mpx_wifi_network_t* network, const char* password, size_t length, bool automatic,
                                 bool restore_static) {
    wifi_mgmr_sta_connect_params_t params = {0};
    memcpy(params.ssid, network->ssid, network->length);
    params.ssid_len = network->length;
    if (length) memcpy(params.key, password, length);
    params.key_len = length;
    const uint8_t* bssid = network->bssid;
    snprintf(params.bssid_str, sizeof(params.bssid_str), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2],
             bssid[3], bssid[4], bssid[5]);
    if (automatic) params.freq1 = wifi_channel_frequency(network->channel);
    params.use_dhcp = !restore_static;
    params.dhcp_timeout_event_only = 1;
    int error = wifi_mgmr_sta_connect(&params);
    mpx_wifi_clear_credentials(params.key, sizeof(params.key));
    if (error) {
        taskENTER_CRITICAL();
        if (schedule_wifi_fallback()) {
            error = 0;
        } else if (status.wifi == MPX_WIFI_JOINING) {
            finish_wifi_disconnect(error);
            ++status.revision;
        }
        taskEXIT_CRITICAL();
    }
    return error;
}

static int mpx_wifi_join(const mpx_wifi_network_t* network, const char* password, size_t length,
                         const mpx_wifi_profile_t* profile, uint8_t fallback) {
    if (!network->supported || network->length == 0 || network->length > 32 ||
        (network->open ? length != 0 : length < 8 || length > 63) || (length && memchr(password, 0, length)))
        return -EINVAL;
    const bool restore_static = profile && profile->ipv4.addr;
    taskENTER_CRITICAL();
    const bool allowed = wifi_scan_kind == WIFI_SCAN_NONE && wifi_idle() && (!profile || !command.method);
    if (allowed) {
        wifi_join_pending = true;
        wifi_restore_static = restore_static;
        if (restore_static) wifi_restore_ipv4 = profile->ipv4;
        if (fallback != MPX_WIFI_NO_INDEX) {
            wifi_fallback.index = fallback;
            wifi_fallback.length = length;
            if (length) memcpy(wifi_fallback.password, password, length);
        }
        status.wifi = MPX_WIFI_JOINING;
        status.wifi_error = 0;
        ++status.revision;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    wifi_address_waiting = false;
    const int error = start_wifi_connection(network, password, length, profile != NULL, restore_static);
    // Automatic attempts report driver failures through the state and advance to the next candidate.
    return profile ? 0 : error;
}

static void retry_wifi_connection(void) {
    taskENTER_CRITICAL();
    if (!wifi_fallback.ready || command.method) {
        taskEXIT_CRITICAL();
        return;
    }
    const uint8_t index = wifi_fallback.index;
    const uint8_t length = wifi_fallback.length;
    char password[sizeof(wifi_fallback.password)];
    memcpy(password, wifi_fallback.password, length);
    clear_wifi_fallback();
    taskEXIT_CRITICAL();
    (void)start_wifi_connection(&wifi_networks[index], password, length, false, false);
    mpx_wifi_clear_credentials(password, sizeof(password));
}

static int disconnect_wifi(bool manual, int reason) {
    taskENTER_CRITICAL();
    const bool active =
        !wifi_disconnect_pending && (manual ? status.wifi == MPX_WIFI_ONLINE || status.wifi == MPX_WIFI_JOINING ||
                                                  status.wifi == MPX_WIFI_DHCP || status.wifi == MPX_WIFI_FAILED
                                            : status.wifi == MPX_WIFI_DHCP);
    const bool cancel_retry = manual && wifi_fallback.ready;
    if (active) {
        if (manual) wifi_join_pending = false;
        if (cancel_retry) {
            finish_wifi_disconnect(0);
            ++status.revision;
        } else {
            wifi_disconnect_pending = true;
            wifi_disconnect_error = reason;
        }
    }
    taskEXIT_CRITICAL();
    if (!active) return -EINVAL;
    if (cancel_retry) return 0;

    const int error = wifi_sta_disconnect();
    if (error) {
        taskENTER_CRITICAL();
        if (wifi_disconnect_pending) {
            wifi_disconnect_pending = false;
            if (!manual) {
                finish_wifi_disconnect(reason);
                ++status.revision;
            }
        }
        taskEXIT_CRITICAL();
    }
    return error;
}

static void poll_wifi_ip(uint32_t now_ms) {
    taskENTER_CRITICAL();
    const bool waiting = status.wifi == MPX_WIFI_DHCP && !wifi_disconnect_pending;
    const bool timed_out = wifi_ip_timeout;
    const bool apply_static = waiting && wifi_apply_static;
    wifi_apply_static = false;
    wifi_ip_timeout = false;
    taskEXIT_CRITICAL();
    if (!waiting) {
        wifi_address_waiting = false;
        return;
    }
    if (!wifi_address_waiting) {
        wifi_address_waiting = true;
        wifi_address_deadline_ms = now_ms + WIFI_ADDRESS_TIMEOUT_MS;
    }
    if (apply_static && wifi_mgmr_sta_ip_set(wifi_restore_ipv4.addr, wifi_restore_ipv4.mask, wifi_restore_ipv4.gateway,
                                             wifi_restore_ipv4.dns)) {
        puts("MicroPixel Wi-Fi static IPv4 restore failed");
        (void)disconnect_wifi(false, -EIO);
    } else if (timed_out || (int32_t)(now_ms - wifi_address_deadline_ms) >= 0) {
        (void)disconnect_wifi(false, CODE_WIFI_ON_GOT_IP_TIMEOUT);
    }
}

static void poll_wifi_reconnect(uint32_t now_ms) {
    if (wifi_scan_waiting && (int32_t)(now_ms - wifi_next_scan_ms) >= 0) wifi_scan_waiting = false;

    taskENTER_CRITICAL();
    const bool idle = wifi_idle() && wifi_scan_kind == WIFI_SCAN_NONE && !command.method;
    taskEXIT_CRITICAL();
    if (!idle) return;

    if (wifi_background_count) {
        const mpx_wifi_profile_t* profile;
        while ((profile = mpx_wifi_history_profile(wifi_next_profile)) != NULL) {
            const mpx_wifi_network_t* network =
                mpx_wifi_profile_match(profile, wifi_background_networks, wifi_background_count);
            if (network) {
                const int error =
                    mpx_wifi_join(network, profile->password, strlen(profile->password), profile, MPX_WIFI_NO_INDEX);
                if (error == -EBUSY) return;
                ++wifi_next_profile;
                return;
            }
            ++wifi_next_profile;
        }
        const mpx_wifi_network_t* network =
            wifi_timebox_profile.ssid[0]
                ? mpx_wifi_profile_match(&wifi_timebox_profile, wifi_background_networks, wifi_background_count)
                : NULL;
        if (network && mpx_wifi_join(network, wifi_timebox_profile.password, strlen(wifi_timebox_profile.password),
                                     &wifi_timebox_profile, MPX_WIFI_NO_INDEX) == -EBUSY)
            return;
        wifi_background_count = 0;
    }
    if (!wifi_scan_waiting && (mpx_wifi_history_profile(0) || wifi_timebox_profile.ssid[0])) {
        delay_wifi_scan(now_ms, WIFI_SCAN_RETRY_MS);
        (void)mpx_wifi_scan(false);
    }
}

#endif

#if defined(CONFIG_BLUETOOTH)
static int mpx_ble_scan(uint32_t now_ms) {
    taskENTER_CRITICAL();
    const bool allowed = status.ble == MPX_BLE_IDLE;
    if (allowed) {
        status.ble = MPX_BLE_SCANNING;
        status.ble_count = 0;
        ++status.ble_info.scan_revision;
        status.ble_info.flags &= ~MPX_BLE_SCAN_COMPLETE;
        ble_scan_end = now_ms + 6000;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    const struct bt_le_scan_param params = {
        .type = BT_LE_SCAN_TYPE_ACTIVE, .interval = BT_GAP_SCAN_FAST_INTERVAL, .window = BT_GAP_SCAN_FAST_WINDOW};
    const int error = bt_le_scan_start(&params, ble_result);
    if (error) {
        taskENTER_CRITICAL();
        status.ble = MPX_BLE_IDLE;
        taskEXIT_CRITICAL();
    }
    return error;
}

static int mpx_ble_stop_scan(void) {
    taskENTER_CRITICAL();
    const bool scanning = status.ble == MPX_BLE_SCANNING;
    taskEXIT_CRITICAL();
    if (!scanning) return 0;
    const int error = bt_le_scan_stop();
    if (error && error != -EALREADY) return error;
    taskENTER_CRITICAL();
    status.ble = MPX_BLE_IDLE;
    status.ble_error = 0;
    status.ble_info.flags |= MPX_BLE_SCAN_COMPLETE;
    ++status.revision;
    const unsigned found = status.ble_count;
    taskEXIT_CRITICAL();
    printf("MicroPixel BLE scan complete, %u connectable devices\n", found);
    return 0;
}

static int mpx_ble_start_advertising(void) {
    taskENTER_CRITICAL();
    const bool resume = ble_resume_advertising;
    const bool allowed = resume || status.ble == MPX_BLE_IDLE;
    if (allowed) {
        ble_resume_advertising = false;
        status.ble_info.flags |= MPX_BLE_DISCOVERY_ENABLED;
        status.ble = MPX_BLE_ADVERTISING;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EBUSY;
    const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
    const struct bt_data ad[] = {
        {.type = BT_DATA_FLAGS, .data_len = sizeof(flags), .data = &flags},
        {.type = BT_DATA_NAME_COMPLETE,
         .data_len = sizeof(CONFIG_BT_DEVICE_NAME) - 1,
         .data = (const uint8_t*)CONFIG_BT_DEVICE_NAME},
#if defined(MICROPIXEL_ORION_BLE_PROFILES)
        BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x0a, 0x18, 0x0d, 0x18, 0x0f, 0x18),
#else
        BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x0a, 0x18),
#endif
    };
    const struct bt_le_adv_param params = {.options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_ONE_TIME,
                                           .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
                                           .interval_max = BT_GAP_ADV_FAST_INT_MAX_2};
#if defined(MICROPIXEL_ORION_BLE_SMOKE)
    const struct bt_data scan_response[] = {BT_DATA_BYTES(BT_DATA_UUID128_ALL, MPX_BLE_SMOKE_SERVICE_UUID)};
    const size_t scan_response_count = ARRAY_SIZE(scan_response);
#else
    const struct bt_data* scan_response = NULL;
    const size_t scan_response_count = 0;
#endif
    const int error = bt_le_adv_start(&params, ad, ARRAY_SIZE(ad), scan_response, scan_response_count);
    if (error) {
        taskENTER_CRITICAL();
        if (status.ble == MPX_BLE_ADVERTISING) {
            status.ble = MPX_BLE_IDLE;
            status.ble_info.flags &= ~MPX_BLE_DISCOVERY_ENABLED;
            ble_resume_advertising = false;
        }
        taskEXIT_CRITICAL();
        printf("MicroPixel BLE start advertising failed (%d)\n", error);
    } else {
        puts(resume ? "MicroPixel BLE connectable advertising resumed"
                    : "MicroPixel BLE connectable advertising started");
    }
    return error;
}

static int mpx_ble_stop_advertising(void) {
    taskENTER_CRITICAL();
    const bool stop_radio = status.ble == MPX_BLE_ADVERTISING && !ble_resume_advertising;
    status.ble_info.flags &= ~MPX_BLE_DISCOVERY_ENABLED;
    ble_resume_advertising = false;
    if (status.ble == MPX_BLE_ADVERTISING) status.ble = MPX_BLE_IDLE;
    taskEXIT_CRITICAL();
    // A connectable advertisement stops when the controller accepts an incoming link.
    if (!stop_radio) return 0;
    const int error = bt_le_adv_stop();
    if (error) {
        taskENTER_CRITICAL();
        status.ble = MPX_BLE_ADVERTISING;
        status.ble_info.flags |= MPX_BLE_DISCOVERY_ENABLED;
        taskEXIT_CRITICAL();
        printf("MicroPixel BLE stop advertising failed (%d)\n", error);
    }
    return error;
}

static int mpx_ble_connect(uint32_t index, uint32_t scan_revision) {
    bt_addr_le_t peer;
    taskENTER_CRITICAL();
    const bool allowed =
        status.ble == MPX_BLE_IDLE && scan_revision == status.ble_info.scan_revision && index < status.ble_count;
    if (allowed) {
        const mpx_ble_device_t* device = &status.devices[index];
        peer.type = device->address_type;
        memcpy(peer.a.val, device->mac, sizeof(peer.a.val));
        memcpy(status.ble_info.peer_name, device->scan.name, sizeof(status.ble_info.peer_name));
        memcpy(status.ble_info.peer_address, device->address, sizeof(status.ble_info.peer_address));
        status.ble = MPX_BLE_CONNECTING;
    }
    taskEXIT_CRITICAL();
    if (!allowed) return -EINVAL;
    struct bt_conn* conn = bt_conn_create_le(&peer, BT_LE_CONN_PARAM_DEFAULT);
    if (conn) {
        taskENTER_CRITICAL();
        const bool pending = status.ble == MPX_BLE_CONNECTING;
        if (pending) ble_link = conn;
        taskEXIT_CRITICAL();
        if (!pending) bt_conn_unref(conn);
        return 0;
    }
    taskENTER_CRITICAL();
    status.ble = MPX_BLE_IDLE;
    status.ble_info.peer_name[0] = 0;
    status.ble_info.peer_address[0] = 0;
    taskEXIT_CRITICAL();
    return -EIO;
}

static int mpx_ble_disconnect(void) {
    taskENTER_CRITICAL();
    struct bt_conn* conn =
        ble_link && !(status.ble_info.flags & MPX_BLE_DISCONNECT_PENDING) ? bt_conn_ref(ble_link) : NULL;
    if (conn) status.ble_info.flags |= MPX_BLE_DISCONNECT_PENDING;
    taskEXIT_CRITICAL();
    if (conn == NULL) return -ENOTCONN;
    const int error = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if (error) {
        taskENTER_CRITICAL();
        status.ble_info.flags &= ~MPX_BLE_DISCONNECT_PENDING;
        taskEXIT_CRITICAL();
    }
    bt_conn_unref(conn);
    return error;
}

#endif

int mpx_wireless_command(uint32_t method, uint32_t index, uint32_t scan_revision, const char* password,
                         uint8_t length) {
    if (method < MICROPIXEL_WIRELESS_WIFI_SCAN || method > MICROPIXEL_WIRELESS_BLE_STOP_SCAN ||
        method == MICROPIXEL_WIRELESS_BLE_INFO || length > sizeof(command.password) || (length && password == NULL))
        return -EINVAL;
    taskENTER_CRITICAL();
    const bool accepted = command.method == 0;
    if (accepted) {
        command.method = method;
        command.index = index;
        command.scan_revision = scan_revision;
        command.length = length;
        if (length) memcpy(command.password, password, length);
        if (method >= MICROPIXEL_WIRELESS_BLE_SCAN) {
            status.ble_error = 0;
            status.ble_info.flags |= MPX_BLE_COMMAND_PENDING;
            ++status.revision;
        }
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
            wifi_background_count = 0;
            delay_wifi_scan(now_ms, WIFI_MANUAL_GRACE_MS);
            error = mpx_wifi_scan(true);
            break;
        case MICROPIXEL_WIRELESS_WIFI_JOIN: {
            if (wifi_scan_kind == WIFI_SCAN_BACKGROUND) return;
            if (command.scan_revision != status.wifi_scan_revision || index >= status.wifi_count) {
                error = -EINVAL;
                break;
            }
            wifi_background_count = 0;
            delay_wifi_scan(now_ms, WIFI_MANUAL_GRACE_MS);
            const mpx_wifi_network_t* selected = &wifi_networks[index];
            error = mpx_wifi_join(&wifi_networks[selected->preferred], command.password, command.length, NULL,
                                  selected->fallback);
            break;
        }
        case MICROPIXEL_WIRELESS_WIFI_LEAVE:
            wifi_background_count = 0;
            delay_wifi_scan(now_ms, WIFI_MANUAL_GRACE_MS);
            error = disconnect_wifi(true, 0);
            break;
#endif
#if defined(CONFIG_BLUETOOTH)
        case MICROPIXEL_WIRELESS_BLE_SCAN:
            error = mpx_ble_scan(now_ms);
            break;
        case MICROPIXEL_WIRELESS_BLE_ADVERTISE:
            error = mpx_ble_start_advertising();
            break;
        case MICROPIXEL_WIRELESS_BLE_HIDE:
            error = mpx_ble_stop_advertising();
            break;
        case MICROPIXEL_WIRELESS_BLE_CONNECT:
            error = mpx_ble_connect(index, command.scan_revision);
            break;
        case MICROPIXEL_WIRELESS_BLE_DISCONNECT:
            error = mpx_ble_disconnect();
            break;
        case MICROPIXEL_WIRELESS_BLE_STOP_SCAN:
            error = mpx_ble_stop_scan();
            break;
#endif
        default:
            error = -ENOTSUP;
            break;
    }
    for (volatile char* p = command.password; p < command.password + sizeof(command.password); ++p) *p = 0;
    taskENTER_CRITICAL();
    if (method >= MICROPIXEL_WIRELESS_BLE_SCAN) {
        status.ble_info.flags &= ~MPX_BLE_COMMAND_PENDING;
        if (error) status.ble_error = error;
        ++status.revision;
    } else if (error) {
        status.wifi_error = error;
        ++status.revision;
    }
    command.method = 0;
    taskEXIT_CRITICAL();
}
