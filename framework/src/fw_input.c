/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input 实现
 *
 * 全局输入路由：
 *   BOOT 键单击 → 返回、双击 → 回桌面、长按 → 电源菜单（锁屏时只保留长按）
 *   滑动手势（periph_touch → svc_power 发布）→ 下拉打开通知中心、上滑打开控制中心
 *   （锁屏时上滑解锁）、左右返回上一级
 * 通知中心 / 控制中心 / 返回 / 主页的按钮都在状态栏（fw_statusbar）。
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

    /* 熄屏时按键只负责唤醒，不再触发导航 */
    if (svc_power_is_sleeping()) {
        svc_power_wake();
        return;
    }

    /* 锁屏时只保留长按电源菜单，避免误触导航 */
    if (fw_lockscreen_is_locked()) {
        if (evt == PERIPH_BTN_EVT_LONG_PRESS || evt == PERIPH_BTN_EVT_VERY_LONG_PRESS) {
            show_power_menu();
        }
        return;
    }

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

/* ------------------------------- 滑动手势 ------------------------------- */

/* 手势由 periph_touch 识别、svc_power 发布；这里统一路由。
 * 锁屏时只响应上滑（解锁），其余手势忽略，避免绕过锁屏。 */
static void evt_swipe_down(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    if (fw_lockscreen_is_locked()) return;

    if (fw_notification_is_visible()) {
        fw_notification_hide();
    } else {
        fw_control_center_hide();   /* 同一时刻只保留一个浮层 */
        fw_notification_show();
    }
}

static void evt_swipe_up(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    if (fw_lockscreen_is_locked()) {
        fw_lockscreen_unlock();
        return;
    }

    if (fw_control_center_is_visible()) {
        fw_control_center_hide();
    } else {
        fw_notification_hide();
        fw_control_center_show();
    }
}

static void evt_swipe_lr(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    if (fw_lockscreen_is_locked()) return;

    /* 浮层开着时先把浮层收掉，再考虑返回上一级 */
    if (fw_notification_is_visible()) {
        fw_notification_hide();
    } else if (fw_control_center_is_visible()) {
        fw_control_center_hide();
    } else {
        fw_app_mgr_back();
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

    svc_event_bus_subscribe(SVC_EVENT_GESTURE_SWIPE_DOWN, evt_swipe_down, NULL);
    svc_event_bus_subscribe(SVC_EVENT_GESTURE_SWIPE_UP, evt_swipe_up, NULL);
    svc_event_bus_subscribe(SVC_EVENT_GESTURE_SWIPE_LEFT, evt_swipe_lr, NULL);
    svc_event_bus_subscribe(SVC_EVENT_GESTURE_SWIPE_RIGHT, evt_swipe_lr, NULL);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}
