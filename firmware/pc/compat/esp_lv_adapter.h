#pragma once
#include "esp_err.h"
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lv_adapter_lock(int timeout_ms);
void esp_lv_adapter_unlock(void);
static inline esp_err_t esp_lv_adapter_request_wake(void) { return ESP_OK; }
static inline esp_err_t esp_lv_adapter_set_dummy_draw(lv_display_t* d, bool enabled) {
    (void)d;
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}
static inline esp_err_t esp_lv_adapter_dummy_draw_blit(lv_display_t* d, int x, int y, int x2, int y2, const void* p,
                                                       bool wait) {
    (void)d;
    (void)x;
    (void)y;
    (void)x2;
    (void)y2;
    (void)p;
    (void)wait;
    return ESP_ERR_NOT_SUPPORTED;
}
#ifdef __cplusplus
}
#endif
