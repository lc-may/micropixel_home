// SPDX-License-Identifier: Apache-2.0
#include "ble_profiles.h"

#include <stdbool.h>

#include "FreeRTOS.h"
#include "bt_uuid.h"
#include "gatt.h"
#include "task.h"

enum { MEASUREMENT_VALUE = 2, SAMPLE_INTERVAL_MS = 1000 };
static uint8_t battery_level = 100;
static bool registered;

static ssize_t read_battery(struct bt_conn* conn, const struct bt_gatt_attr* attr, void* buffer, uint16_t length,
                            uint16_t offset) {
    taskENTER_CRITICAL();
    const uint8_t level = battery_level;
    taskEXIT_CRITICAL();
    return bt_gatt_attr_read(conn, attr, buffer, length, offset, &level, sizeof(level));
}

static struct bt_gatt_attr battery_attributes[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
    BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                           read_battery, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_attr heart_rate_attributes[] = {
    BT_GATT_PRIMARY_SERVICE(BT_UUID_HRS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HRS_MEASUREMENT, BT_GATT_CHRC_NOTIFY, 0, NULL, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
};
static struct bt_gatt_service battery_service = BT_GATT_SERVICE(battery_attributes);
static struct bt_gatt_service heart_rate_service = BT_GATT_SERVICE(heart_rate_attributes);

int mpx_ble_profiles_init(void) {
    int error = bt_gatt_service_register(&battery_service);
    if (error) return error;
    error = bt_gatt_service_register(&heart_rate_service);
    if (error) {
        (void)bt_gatt_service_unregister(&battery_service);
        return error;
    }
    taskENTER_CRITICAL();
    registered = true;
    taskEXIT_CRITICAL();
    return 0;
}

void mpx_ble_profiles_poll(uint32_t now_ms) {
    static uint32_t next_sample;
    static uint8_t bpm = 60;
    taskENTER_CRITICAL();
    const bool ready = registered;
    taskEXIT_CRITICAL();
    if (!ready || (int32_t)(now_ms - next_sample) < 0) return;
    next_sample = now_ms + SAMPLE_INTERVAL_MS;

    // Simulate changing inputs for clients exercising reads and CCC notifications.
    taskENTER_CRITICAL();
    battery_level = battery_level == 0 ? 100 : battery_level - 1;
    const uint8_t level = battery_level;
    taskEXIT_CRITICAL();
    const uint8_t measurement[] = {0, bpm};
    bpm = bpm == 80 ? 60 : bpm + 1;
    (void)bt_gatt_notify(NULL, &battery_attributes[MEASUREMENT_VALUE], &level, sizeof(level));
    (void)bt_gatt_notify(NULL, &heart_rate_attributes[MEASUREMENT_VALUE], measurement, sizeof(measurement));
}
