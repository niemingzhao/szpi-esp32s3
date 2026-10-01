/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Window 实现
 */

#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.window";

static lv_obj_t *s_active = NULL;

esp_err_t fw_window_init(void)
{
    lvgl_port_lock(0);
    s_active = lv_screen_active();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t fw_window_switch_to(lv_obj_t *scr, lv_scr_load_anim_t anim, uint32_t time_ms)
{
    if (scr == NULL) return ESP_ERR_INVALID_ARG;
    /* s_active 可能是已被删除的屏（地址会被堆复用），必须再和 LVGL 实际的活动屏核对 */
    if (scr == s_active && scr == lv_screen_active()) return ESP_OK;

    lv_scr_load_anim(scr, anim, time_ms, 0, false);
    s_active = scr;
    return ESP_OK;
}

esp_err_t fw_window_sync_active(void)
{
    s_active = lv_screen_active();
    return ESP_OK;
}

lv_obj_t *fw_window_active(void)
{
    return s_active;
}
