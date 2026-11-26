// SPDX-License-Identifier: Apache-2.0
#include "bflb_gpio.h"
#include "bl618dg_glb.h"
#include "board_config.h"
#include "board_gpio.h"
#include "config/orion_pins.h"

_Static_assert(ORION_I2S_MCLK_PIN != ORION_LCD_RESET_PIN, "Orion routes MCLK and LCD RESET to separate GPIOs");

void mpx_orion_i2s_codec_gpio_init(void);

void mpx_orion_i2s_codec_gpio_init(void) {
    board_i2s_gpio_init();
    // Timebox clocks the JY6316 from BCLK. Keep GPIO33/BOOT free of CLKOUT.
    GLB_Set_I2S_CLK(ENABLE, 0, GLB_I2S_DI_SEL_I2S_DI_INPUT, GLB_I2S_DO_SEL_I2S_DO_OUTPT);

    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    bflb_gpio_init(gpio, BOARD_AUDIO_PA_PIN, GPIO_OUTPUT | GPIO_PULLDOWN | GPIO_SMT_EN | GPIO_DRV_1);
    bflb_gpio_set(gpio, BOARD_AUDIO_PA_PIN);
}
