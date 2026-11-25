// SPDX-License-Identifier: Apache-2.0
#include "display_smoke.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "bflb_mtimer.h"
#include "lv_port_disp_default.h"
#include "lvgl.h"
#include "task.h"

static uint32_t display_tick_ms(void) { return (uint32_t)bflb_mtimer_get_time_ms(); }

static void display_smoke(void *argument) {
    (void)argument;
    lv_init();
    lv_tick_set_cb(display_tick_ms);
    lv_display_t *display = lv_port_disp_init();
    if (!display) {
        puts("MicroPixel display initialization failed");
        vTaskDelete(NULL);
        return;
    }

    static const uint32_t colors[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    for (uint32_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        lv_obj_t *bar = lv_obj_create(screen);
        lv_obj_remove_style_all(bar);
        lv_obj_set_pos(bar, lv_pct(25 * (int32_t)i), 0);
        lv_obj_set_size(bar, lv_pct(25), lv_pct(85));
        lv_obj_set_style_bg_color(bar, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text_static(label, "MicroPixel Orion");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    printf("MicroPixel display initialized: %ld x %ld\n", (long)lv_display_get_horizontal_resolution(display),
           (long)lv_display_get_vertical_resolution(display));
    for (;;) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void display_smoke_start(void) {
    if (xTaskCreate(display_smoke, "mpx_display", 2048, NULL, 2, NULL) != pdPASS) {
        puts("MicroPixel display task creation failed");
    }
}

#ifdef MPX_NATIVE_VIDEO_TEST
int main(void) {
    display_smoke_start();
    return 0;
}
#endif
