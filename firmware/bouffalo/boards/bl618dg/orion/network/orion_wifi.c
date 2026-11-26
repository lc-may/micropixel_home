// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>

#include "bflb_gpio.h"
#include "board_rf.h"
#include "config/orion_pins.h"
#include "sysinit.h"

static void orion_wifi_antenna(const void* unused) {
    (void)unused;
    struct bflb_device_s* gpio = bflb_device_get_by_name("gpio");
    if (gpio == NULL) abort();
    // BL-M65P-32S wires its internal RF switch to IO16. For the SDK's
    // even-GPIO SPDT wiring, low selects the 2.4 GHz Wi-Fi path.
    bflb_gpio_reset(gpio, ORION_RF_SWITCH_PIN);
    bflb_gpio_init(gpio, ORION_RF_SWITCH_PIN, GPIO_FUNC_GPIO | GPIO_OUTPUT | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_reset(gpio, ORION_RF_SWITCH_PIN);
    const int status = board_rf_combo_init();
    if (status != 0) {
        printf("Orion Wi-Fi RF initialization failed (%d)\n", status);
        abort();
    }
    puts("Orion Wi-Fi RF switch on GPIO16 selected before FHOST startup");
}

SYSINIT(orion_wifi_antenna, SI_SUB_EVENTS, SI_ORDER_LAST, orion_wifi_antenna, NULL);
