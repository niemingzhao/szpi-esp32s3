/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Control Center 实现
 *
 * 由 fw_input 在“上滑”手势时打开；点击遮罩关闭。
 * 蓝牙 / 手电筒 / 锁屏磁贴待对应服务（BLE、GPIO、锁屏）落地后再补。
 */

#include "fw_control_center.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.ctrl_center";

#define CC_PANEL_H   180

static lv_obj_t *s_scrim = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_bright = NULL;
static lv_obj_t *s_vol = NULL;
static bool s_visible = false;

static void update_wifi(void)
{
    if (s_wifi_label == NULL) return;

    svc_net_status_t st;
    bool connected = (svc_net_get_status(&st) == ESP_OK) && st.wifi_connected;

    lv_label_set_text(s_wifi_label, connected ? (LV_SYMBOL_WIFI " 已连接")
                                              : (LV_SYMBOL_WIFI " 未连接"));
}

static void forget_confirmed(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    svc_net_wifi_forget();
    update_wifi();
}

static void wifi_long_press_cb(lv_event_t *e)
{
    (void)e;
    fw_ui_dialog(NULL, "Wi-Fi", "清除已保存的 Wi-Fi 凭据？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, forget_confirmed, NULL);
}

static void scrim_cb(lv_event_t *e)
{
    (void)e;
    fw_control_center_hide();
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;

    svc_net_status_t st;
    if (svc_net_get_status(&st) == ESP_OK && st.wifi_connected) {
        svc_net_wifi_stop();
    } else {
        svc_net_wifi_start(SVC_NET_MODE_STA);
    }
    update_wifi();
}

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    svc_power_set_brightness((uint8_t)lv_slider_get_value(s));
}

static void vol_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    svc_audio_set_volume((uint8_t)lv_slider_get_value(s));
}

static void audition_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = svc_audio_play_tone_async(1000, 300);
    if (err != ESP_OK) {
        fw_ui_toast("音频不可用", 2000);
    }
}

static void evt_wifi(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    lvgl_port_lock(0);
    update_wifi();
    lvgl_port_unlock();
}

esp_err_t fw_control_center_init(void)
{
    lvgl_port_lock(0);

    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scrim, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);
    lv_obj_add_event_cb(s_scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_scrim);
    lv_obj_set_size(panel, lv_disp_get_hor_res(NULL), CC_PANEL_H);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "控制中心");
    lv_obj_set_style_text_font(title, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 8);

    lv_obj_t *wifi = lv_btn_create(panel);
    lv_obj_set_size(wifi, 152, 44);
    lv_obj_align(wifi, LV_ALIGN_TOP_LEFT, 10, 36);
    lv_obj_set_style_radius(wifi, 8, 0);
    lv_obj_set_style_shadow_width(wifi, 0, 0);
    lv_obj_add_event_cb(wifi, wifi_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(wifi, wifi_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_wifi_label = lv_label_create(wifi);
    lv_obj_set_style_text_font(s_wifi_label, fw_asset_font_20(), 0);
    lv_obj_center(s_wifi_label);

    lv_obj_t *bl = lv_label_create(panel);
    lv_label_set_text(bl, "亮度");
    lv_obj_set_style_text_font(bl, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(bl, fw_theme_color_text_secondary(), 0);
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 10, 92);

    s_bright = lv_slider_create(panel);
    lv_obj_set_size(s_bright, 240, 12);
    lv_obj_align(s_bright, LV_ALIGN_TOP_LEFT, 62, 96);
    lv_slider_set_range(s_bright, 0, 100);
    lv_slider_set_value(s_bright, (int32_t)svc_power_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *vl = lv_label_create(panel);
    lv_label_set_text(vl, "音量");
    lv_obj_set_style_text_font(vl, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(vl, fw_theme_color_text_secondary(), 0);
    lv_obj_align(vl, LV_ALIGN_TOP_LEFT, 10, 118);

    s_vol = lv_slider_create(panel);
    lv_obj_set_size(s_vol, 240, 12);
    lv_obj_align(s_vol, LV_ALIGN_TOP_LEFT, 62, 122);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, (int32_t)svc_audio_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_vol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 试听：验证音频通路（ES8311 + I2S + 功放） */
    lv_obj_t *audition = lv_btn_create(panel);
    lv_obj_set_size(audition, 84, 26);
    lv_obj_align(audition, LV_ALIGN_TOP_LEFT, 10, 146);
    lv_obj_set_style_radius(audition, 6, 0);
    lv_obj_set_style_shadow_width(audition, 0, 0);
    lv_obj_add_event_cb(audition, audition_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(audition);
    lv_label_set_text(al, "试听");
    lv_obj_set_style_text_font(al, fw_asset_font_20(), 0);
    lv_obj_center(al);

    update_wifi();

    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    lvgl_port_unlock();

    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, evt_wifi, NULL);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t fw_control_center_show(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    update_wifi();
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_control_center_hide(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_control_center_toggle(void)
{
    return s_visible ? fw_control_center_hide() : fw_control_center_show();
}

bool fw_control_center_is_visible(void)
{
    return s_visible;
}
