// SPDX-License-Identifier: Apache-2.0
#include "ble_smoke.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "bluetooth.h"
#include "gatt.h"
#include "platform/wireless/wireless.h"
#include "shell.h"
#include "task.h"

#if defined(MICROPIXEL_ORION_RX_BENCHMARK)
#include "hci_core.h"
#include "misc/byteorder.h"
#endif

#define VALUE_UUID BT_UUID_128_ENCODE(0x79180b9c, 0xf9dc, 0x47b2, 0xa71b, 0x4cd98f201202)

enum { VALUE_CAPACITY = 20 };
static uint8_t value[VALUE_CAPACITY] = "Orion BLE";
static uint16_t value_size = sizeof("Orion BLE") - 1;
static bool notify_pending;

static ssize_t read_value(struct bt_conn* conn, const struct bt_gatt_attr* attr, void* buffer, uint16_t length,
                          uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buffer, length, offset, value, value_size);
}

static ssize_t write_value(struct bt_conn* conn, const struct bt_gatt_attr* attr, const void* buffer, uint16_t length,
                           uint16_t offset, uint8_t flags) {
    (void)conn;
    (void)attr;
    (void)flags;
    if (offset != 0) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (length == 0 || length > sizeof(value)) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    taskENTER_CRITICAL();
    memcpy(value, buffer, length);
    value_size = length;
    notify_pending = true;
    taskEXIT_CRITICAL();
    return length;
}

enum { VALUE_ATTRIBUTE = 2 };
static struct bt_gatt_attr attributes[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(MPX_BLE_SMOKE_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(
        BT_UUID_DECLARE_128(VALUE_UUID),
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, read_value, write_value, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_service service = BT_GATT_SERVICE(attributes);

#if defined(MICROPIXEL_ORION_RX_BENCHMARK)
#define THROUGHPUT_UUID(id) BT_UUID_DECLARE_128(BT_UUID_128_ENCODE(0x79180b9c, 0xf9dc, 0x47b2, 0xa71b, id))

enum { BENCH_RX = 1, BENCH_TX = 2, BENCH_TUNE = 4, BENCH_CAPACITY = 244, BENCH_TX_ATTRIBUTE = 4 };
static struct bt_conn* bench_conn;
static TaskHandle_t bench_task;
static unsigned bench_mode;
static unsigned bench_generation;
static uint8_t bench_pattern[BENCH_CAPACITY - sizeof(uint32_t)];
static uint8_t bench_packet[BENCH_CAPACITY];
static struct {
    uint32_t rx_frames, rx_bytes, rx_next, rx_gaps, rx_invalid, tx_frames, tx_bytes, tx_errors;
} bench_stats;

static ssize_t bench_receive(struct bt_conn* conn, const struct bt_gatt_attr* attr, const void* buffer, uint16_t length,
                             uint16_t offset, uint8_t flags) {
    (void)attr;
    if (offset || !(flags & BT_GATT_WRITE_FLAG_CMD)) return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
    const uint8_t* data = buffer;
    const bool valid = length > sizeof(uint32_t) && length <= BENCH_CAPACITY &&
                       memcmp(data + sizeof(uint32_t), bench_pattern, length - sizeof(uint32_t)) == 0;
    taskENTER_CRITICAL();
    if (conn == bench_conn && (bench_mode & BENCH_RX)) {
        if (!valid || sys_get_le32(data) < bench_stats.rx_next) {
            ++bench_stats.rx_invalid;
        } else {
            const uint32_t sequence = sys_get_le32(data);
            bench_stats.rx_gaps += sequence - bench_stats.rx_next;
            bench_stats.rx_next = sequence + 1;
            ++bench_stats.rx_frames;
            bench_stats.rx_bytes += length;
        }
    }
    taskEXIT_CRITICAL();
    return length;
}

static ssize_t bench_control(struct bt_conn* conn, const struct bt_gatt_attr* attr, const void* buffer, uint16_t length,
                             uint16_t offset, uint8_t flags) {
    (void)attr;
    (void)flags;
    if (offset || length != 1) return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    const uint8_t mode = *(const uint8_t*)buffer;
    if (mode > BENCH_TUNE) return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    taskENTER_CRITICAL();
    if (bench_conn && conn != bench_conn) {
        taskEXIT_CRITICAL();
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }
    if (!bench_conn) bench_conn = bt_conn_ref(conn);
    bench_mode = mode;
    ++bench_generation;
    if (mode && mode < BENCH_TUNE) memset(&bench_stats, 0, sizeof(bench_stats));
    taskEXIT_CRITICAL();
    if (mode) xTaskNotifyGive(bench_task);
    return length;
}

static ssize_t bench_read_count(struct bt_conn* conn, const struct bt_gatt_attr* attr, void* buffer, uint16_t length,
                                uint16_t offset) {
    uint8_t data[sizeof(uint32_t)];
    taskENTER_CRITICAL();
    const uint32_t frames = conn == bench_conn ? bench_stats.rx_frames : 0;
    taskEXIT_CRITICAL();
    sys_put_le32(frames, data);
    return bt_gatt_attr_read(conn, attr, buffer, length, offset, data, sizeof(data));
}

static struct bt_gatt_attr bench_attributes[] = {
    BT_GATT_PRIMARY_SERVICE(THROUGHPUT_UUID(0x4cd98f201203)),
    BT_GATT_CHARACTERISTIC(THROUGHPUT_UUID(0x4cd98f201204), BT_GATT_CHRC_WRITE_WITHOUT_RESP, BT_GATT_PERM_WRITE, NULL,
                           bench_receive, NULL),
    BT_GATT_CHARACTERISTIC(THROUGHPUT_UUID(0x4cd98f201205), BT_GATT_CHRC_NOTIFY, 0, NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(THROUGHPUT_UUID(0x4cd98f201206), BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, bench_read_count, bench_control, NULL),
};
static struct bt_gatt_service bench_service = BT_GATT_SERVICE(bench_attributes);

static void bench_worker(void* unused) {
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL();
        struct bt_conn* conn = bench_conn ? bt_conn_ref(bench_conn) : NULL;
        const unsigned mode = bench_mode;
        const unsigned generation = bench_generation;
        taskEXIT_CRITICAL();
        if (!conn) continue;
        if (mode == BENCH_TUNE) {
            const int interval = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(6, 12, 0, 400));
            const int length = bt_le_set_data_len(conn, 251, 2120);
            const int phy = hci_le_set_phy(conn, 0, BT_HCI_LE_PHY_PREFER_2M, BT_HCI_LE_PHY_PREFER_2M, 0);
            printf("GATT BENCH tune interval_rc=%d length_rc=%d phy_rc=%d\n", interval, length, phy);
        }
        if (mode & BENCH_TX) {
            const struct bt_gatt_attr* attr = &bench_attributes[BENCH_TX_ATTRIBUTE];
            const uint16_t mtu = bt_gatt_get_mtu(conn);
            const uint16_t length = mtu > BENCH_CAPACITY + 3 ? BENCH_CAPACITY : (mtu >= 23 ? mtu - 3 : 0);
            for (uint32_t sequence = 0; length && bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY);) {
                taskENTER_CRITICAL();
                const bool active = generation == bench_generation;
                taskEXIT_CRITICAL();
                if (!active) break;
                sys_put_le32(sequence, bench_packet);
                const int result = bt_gatt_notify(conn, attr, bench_packet, length);
                taskENTER_CRITICAL();
                if (generation == bench_generation) {
                    if (result) {
                        ++bench_stats.tx_errors;
                    } else {
                        ++sequence;
                        ++bench_stats.tx_frames;
                        bench_stats.tx_bytes += length;
                    }
                }
                taskEXIT_CRITICAL();
                if (result) vTaskDelay(1);
            }
        }
        bt_conn_unref(conn);
    }
}

static void bench_disconnected(struct bt_conn* conn, uint8_t reason) {
    (void)reason;
    taskENTER_CRITICAL();
    const bool owned = conn == bench_conn;
    if (owned) {
        bench_conn = NULL;
        bench_mode = 0;
        ++bench_generation;
    }
    taskEXIT_CRITICAL();
    if (owned) bt_conn_unref(conn);
}

static void bench_phy_updated(struct bt_conn* conn, uint8_t tx, uint8_t rx) {
    (void)conn;
    printf("GATT BENCH phy tx=%u rx=%u\n", tx, rx);
}

static void bench_interval_updated(struct bt_conn* conn, uint16_t interval, uint16_t latency, uint16_t timeout) {
    (void)conn;
    printf("GATT BENCH interval=%u latency=%u timeout=%u\n", interval, latency, timeout);
}

static struct bt_conn_cb bench_callbacks = {
    .disconnected = bench_disconnected,
    .le_param_updated = bench_interval_updated,
    .le_phy_updated = bench_phy_updated,
};

static void bench_snapshot(int argc, char** argv) {
    (void)argc;
    (void)argv;
    taskENTER_CRITICAL();
    const __typeof__(bench_stats) stats = bench_stats;
    taskEXIT_CRITICAL();
    printf(
        "GATT BENCH rx_frames=%lu rx_bytes=%lu rx_next=%lu rx_gaps=%lu rx_invalid=%lu "
        "tx_frames=%lu tx_bytes=%lu tx_errors=%lu stack_free=%u\n",
        (unsigned long)stats.rx_frames, (unsigned long)stats.rx_bytes, (unsigned long)stats.rx_next,
        (unsigned long)stats.rx_gaps, (unsigned long)stats.rx_invalid, (unsigned long)stats.tx_frames,
        (unsigned long)stats.tx_bytes, (unsigned long)stats.tx_errors,
        bench_task ? (unsigned)(uxTaskGetStackHighWaterMark(bench_task) * sizeof(StackType_t)) : 0);
}

SHELL_CMD_EXPORT_ALIAS(bench_snapshot, gatt_bench_stats, Orion GATT throughput counters);
#endif

int mpx_ble_smoke_init(void) {
    int result = bt_gatt_service_register(&service);
#if defined(MICROPIXEL_ORION_RX_BENCHMARK)
    if (result) return result;
    memset(bench_pattern, 0xa5, sizeof(bench_pattern));
    memcpy(bench_packet + sizeof(uint32_t), bench_pattern, sizeof(bench_pattern));
    result = bt_gatt_service_register(&bench_service);
    if (result) return result;
    if (xTaskCreate(bench_worker, "gatt_bench", 3072 / sizeof(StackType_t), NULL, 6, &bench_task) != pdPASS)
        return -ENOMEM;
    bt_conn_cb_register(&bench_callbacks);
#endif
    return result;
}

void mpx_ble_smoke_poll(void) {
    uint8_t data[VALUE_CAPACITY];
    uint16_t size = 0;
    taskENTER_CRITICAL();
    if (notify_pending) {
        size = value_size;
        memcpy(data, value, size);
        notify_pending = false;
    }
    taskEXIT_CRITICAL();
    if (size) bt_gatt_notify(NULL, &attributes[VALUE_ATTRIBUTE], data, size);
}

static void ble_command(int argc, char** argv) {
    static const struct {
        const char* name;
        uint32_t method;
    } actions[] = {
        {"advertise", MICROPIXEL_WIRELESS_BLE_ADVERTISE},
        {"hide", MICROPIXEL_WIRELESS_BLE_HIDE},
        {"scan", MICROPIXEL_WIRELESS_BLE_SCAN},
        {"disconnect", MICROPIXEL_WIRELESS_BLE_DISCONNECT},
    };
    if (argc == 2) {
        for (size_t i = 0; i < ARRAY_SIZE(actions); ++i) {
            if (strcmp(argv[1], actions[i].name) != 0) continue;
            const int error = mpx_wireless_command(actions[i].method, 0, 0, NULL, 0);
            if (error) printf("Orion BLE command rejected (%d)\r\n", error);
            return;
        }
    }
    puts("Usage: orion_ble advertise|hide|scan|disconnect");
}

SHELL_CMD_EXPORT_ALIAS(ble_command, orion_ble, Orion BLE diagnostics);
