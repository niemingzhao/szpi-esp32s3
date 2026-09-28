/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input 实现
 *
 * 全局输入路由：BOOT 键单击 → 返回、双击 → 回桌面、长按 → 电源菜单。
 * 不使用滑动手势；通知中心 / 控制中心 / 返回 / 主页的按钮都在状态栏（fw_statusbar）。
 *
 * 说明：按键事件目前尚未接入 svc_event_bus，fw_input 直接注册
 * periph_button 回调（见 docs/02-architecture/01-layer-design.md 例外）。
 */

#include "fw_common.h"
#include "periph_button.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.input";

/* ------------------------------- 电源菜单 ------------------------------- */

static void power_menu_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;

    if (btn == FW_DIALOG_BTN_SHUTDOWN) {
        svc_power_request_shutdown();
    } else if (btn == FW_DIALOG_BTN_REBOOT) {
        svc_power_request_reboot();
    }
}

static void show_power_menu(void)
{
    lv_obj_t *dlg = fw_ui_dialog(NULL, "电源", "请选择操作",
                                 FW_DIALOG_BTN_SHUTDOWN | FW_DIALOG_BTN_REBOOT | FW_DIALOG_BTN_CANCEL,
                                 power_menu_cb, NULL);
    if (dlg == NULL) {
        ESP_LOGW(TAG, "power menu not shown");
    }
}

/* ------------------------------- BOOT 键 ------------------------------- */

static void key_cb(periph_button_evt_t evt, void *user)
{
    (void)user;

    switch (evt) {
    case PERIPH_BTN_EVT_CLICK:
        fw_app_mgr_back();
        break;
    case PERIPH_BTN_EVT_DOUBLE_CLICK:
        fw_app_mgr_back_to_home();
        break;
    case PERIPH_BTN_EVT_LONG_PRESS:
    case PERIPH_BTN_EVT_VERY_LONG_PRESS:
        show_power_menu();
        break;
    default:
        break;
    }
}

/* -------------------------------- 按键 -------------------------------- */

esp_err_t fw_input_init(void)
{
    esp_err_t err = periph_button_register_callback(key_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register button callback failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}
