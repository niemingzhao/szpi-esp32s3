/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - 初始化入口
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.init";

static void theme_rebuild_async(void *user)
{
    (void)user;

    fw_statusbar_rebuild();
    fw_app_mgr_rebuild_all();

    ESP_LOGI(TAG, "UI rebuilt for theme change");
}

/* 换主题：重建全局浮层与所有 App 的界面。
 * 用 lv_async_call 推迟到 LVGL 任务里执行 —— 主题是在 App 的按钮回调里切换的，
 * 同步重建会把"正在处理事件的控件"删掉。 */
static void evt_theme(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    lv_async_call(theme_rebuild_async, NULL);
    lvgl_port_unlock();
}

esp_err_t fw_init(void)
{
    ESP_LOGI(TAG, "=== framework init start ===");

    /* lv_layer_top() 默认带 LV_OBJ_FLAG_CLICKABLE，会吞掉全屏触摸，先清标志；
     * 之后挂在它上面的状态栏 / 浮层仍可正常接收点击。 */
    lvgl_port_lock(0);
    lv_obj_set_clickable(lv_layer_top(), false);
    lv_obj_set_scrollable(lv_layer_top(), false);
    /* 开机画面之前不允许出现任何界面元素：状态栏就挂在这层上，这里先藏起来，
     * 等 fw_boot_animation() 收尾时再打开。放在建状态栏之前，避免它闪一下。 */
    lv_obj_set_hidden(lv_layer_top(), true);
    lvgl_port_unlock();

    ESP_ERROR_CHECK(fw_theme_init());
    ESP_ERROR_CHECK(fw_asset_init());
    ESP_ERROR_CHECK(fw_window_init());
    ESP_ERROR_CHECK(fw_app_mgr_init());
    ESP_ERROR_CHECK(fw_ui_init());
    ESP_ERROR_CHECK(fw_script_init());
    ESP_ERROR_CHECK(fw_statusbar_init());
    ESP_ERROR_CHECK(fw_input_init());

    svc_event_bus_subscribe(SVC_EVENT_THEME_CHANGED, evt_theme, NULL);

    ESP_LOGI(TAG, "=== framework init done ===");
    return ESP_OK;
}
