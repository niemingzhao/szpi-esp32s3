/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input 实现
 *
 * 说明：按键事件目前尚未接入 svc_event_bus，fw_input 直接注册
 * periph_button 回调（见 docs/02-architecture/01-layer-design.md 例外）。
 */

#include "fw_common.h"
#include "periph_button.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.input";

#define FW_NAV_H   28

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
        ESP_LOGI(TAG, "long press -> power menu (TODO)");
        break;
    case PERIPH_BTN_EVT_VERY_LONG_PRESS:
        ESP_LOGI(TAG, "very long press -> power menu (TODO)");
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

static void nav_back_cb(lv_event_t *e)
{
    (void)e;
    fw_app_mgr_back();
}

static void nav_home_cb(lv_event_t *e)
{
    (void)e;
    fw_app_mgr_back_to_home();
}

static void make_nav_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, lv_coord_t x)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 52, FW_NAV_H - 4);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
}

esp_err_t fw_input_create_navbar(void)
{
    lvgl_port_lock(0);

    lv_obj_t *bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bar, lv_disp_get_hor_res(NULL), FW_NAV_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    make_nav_btn(bar, LV_SYMBOL_LEFT, nav_back_cb, 6);
    make_nav_btn(bar, LV_SYMBOL_HOME, nav_home_cb, 66);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "navbar created");
    return ESP_OK;
}
