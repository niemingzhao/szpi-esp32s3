/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input 实现
 *
 * 全局输入路由：BOOT 键单击 → 返回上一级、双击 → 回桌面、长按 → 电源菜单。
 * 触摸的点击 / 长按经 LVGL input device 直达当前界面，不在这里路由。
 *
 * 说明：fw_input 直接注册 periph_button 回调（见 docs/02-architecture/01-layer-design.md
 * 例外），再把按键事件转发到 svc_event_bus 的 SVC_EVENT_KEY，脚本可按 "key" 订阅。
 */

#include "fw_common.h"
#include "periph_button.h"
#include "svc_common.h"
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

    /* 附加动作：把按键事件也发到事件总线，脚本订阅 "key" 即可收到。载荷是
     * uint8_t 的事件序号，与 periph_button_evt_t 的枚举顺序一致（0 单击 / 1 双击 /
     * 2 长按 / 3 极长按），发到脚本侧就是一个 1 字节字符串。
     * svc_event_bus_publish() 按值拷贝，下面照旧走导航，互不影响。 */
    const uint8_t code = (uint8_t)evt;
    svc_event_bus_publish(SVC_EVENT_KEY, &code, sizeof(code));

    /* 熄屏时按键只负责唤醒，不再触发导航 */
    if (svc_power_is_sleeping()) {
        svc_power_wake();
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
