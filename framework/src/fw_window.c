/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Window 实现
 */

#include "fw_common.h"
#include "esp_log.h"

static const char *TAG = "fw.window";

static lv_obj_t *s_active = NULL;

esp_err_t fw_window_init(void)
{
    s_active = lv_scr_act();
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t fw_window_switch_to(lv_obj_t *scr, lv_scr_load_anim_t anim, uint32_t time_ms)
{
    if (scr == NULL) return ESP_ERR_INVALID_ARG;
    if (scr == s_active) return ESP_OK;

    lv_scr_load_anim(scr, anim, time_ms, 0, false);
    s_active = scr;
    return ESP_OK;
}

lv_obj_t *fw_window_active(void)
{
    return s_active;
}
