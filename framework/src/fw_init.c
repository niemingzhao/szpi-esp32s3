/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - 初始化入口
 */

#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.init";

esp_err_t fw_init(void)
{
    ESP_LOGI(TAG, "=== framework init start ===");

    /* lv_layer_top() 默认带 LV_OBJ_FLAG_CLICKABLE，会吞掉全屏触摸，先清标志；
     * 之后挂在它上面的状态栏 / 浮层仍可正常接收点击。 */
    lvgl_port_lock(0);
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lvgl_port_unlock();

    ESP_ERROR_CHECK(fw_theme_init());
    ESP_ERROR_CHECK(fw_asset_init());
    ESP_ERROR_CHECK(fw_window_init());
    ESP_ERROR_CHECK(fw_app_mgr_init());
    ESP_ERROR_CHECK(fw_ui_init());
    ESP_ERROR_CHECK(fw_statusbar_init());
    ESP_ERROR_CHECK(fw_notification_init());
    ESP_ERROR_CHECK(fw_control_center_init());
    ESP_ERROR_CHECK(fw_input_init());
    ESP_ERROR_CHECK(fw_input_create_navbar());

    ESP_LOGI(TAG, "=== framework init done ===");
    return ESP_OK;
}
