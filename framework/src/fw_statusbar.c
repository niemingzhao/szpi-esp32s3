/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Status Bar 实现
 *
 * 挂在 lv_layer_top() 上，不随 App 屏幕切换消失。
 * 事件回调运行在 svc_event_bus 的 dispatcher 任务，访问 LVGL 前必须加锁。
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.statusbar";

#define FW_STATUS_H   24

static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_time = NULL;
static lv_obj_t *s_wifi = NULL;
static lv_obj_t *s_music = NULL;
static lv_obj_t *s_bt = NULL;
static lv_obj_t *s_noti = NULL;
static lv_timer_t *s_timer = NULL;

static void refresh_time(void)
{
    if (s_time == NULL) return;

    if (!svc_time_is_synced()) {
        lv_label_set_text(s_time, "--:--");
        return;
    }

    char buf[8];
    if (svc_time_format(svc_time_now(), "%H:%M", buf, sizeof(buf)) == ESP_OK) {
        lv_label_set_text(s_time, buf);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_time();
}

esp_err_t fw_statusbar_set_time(const char *time)
{
    lvgl_port_lock(0);
    if (s_time != NULL && time != NULL) {
        lv_label_set_text(s_time, time);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected)
{
    (void)rssi;
    lvgl_port_lock(0);
    if (s_wifi != NULL) {
        lv_obj_set_style_text_color(s_wifi,
                                    connected ? fw_theme_color_text_primary()
                                              : fw_theme_color_text_disabled(),
                                    0);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_statusbar_set_music_playing(bool on)
{
    lvgl_port_lock(0);
    if (s_music != NULL) {
        if (on) lv_obj_clear_flag(s_music, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_music, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_statusbar_set_bluetooth(bool on)
{
    lvgl_port_lock(0);
    if (s_bt != NULL) {
        if (on) lv_obj_clear_flag(s_bt, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_bt, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

static void evt_time(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    lvgl_port_lock(0);
    refresh_time();
    lvgl_port_unlock();
}

static void evt_wifi(const svc_event_t *evt, void *user)
{
    (void)user;
    fw_statusbar_set_wifi(0, evt->id == SVC_EVENT_WIFI_CONNECTED);
}

static void evt_audio(const svc_event_t *evt, void *user)
{
    (void)user;
    fw_statusbar_set_music_playing(evt->id == SVC_EVENT_AUDIO_PLAYBACK_STARTED);
}

static void evt_noti(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    if (s_noti != NULL) {
        if (svc_notification_get_count() > 0) lv_obj_clear_flag(s_noti, LV_OBJ_FLAG_HIDDEN);
        else                                  lv_obj_add_flag(s_noti, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

esp_err_t fw_statusbar_init(void)
{
    lvgl_port_lock(0);

    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(NULL), FW_STATUS_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_bar, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_text_font(s_bar, fw_asset_font_20(), 0);

    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_time, LV_ALIGN_LEFT_MID, 6, 0);

    s_wifi = lv_label_create(s_bar);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi, fw_theme_color_text_disabled(), 0);
    lv_obj_align(s_wifi, LV_ALIGN_RIGHT_MID, -6, 0);

    s_music = lv_label_create(s_bar);
    lv_label_set_text(s_music, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(s_music, fw_theme_color_accent(), 0);
    lv_obj_align(s_music, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_obj_add_flag(s_music, LV_OBJ_FLAG_HIDDEN);

    s_bt = lv_label_create(s_bar);
    lv_label_set_text(s_bt, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_bt, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_bt, LV_ALIGN_RIGHT_MID, -50, 0);
    lv_obj_add_flag(s_bt, LV_OBJ_FLAG_HIDDEN);

    s_noti = lv_label_create(s_bar);
    lv_label_set_text(s_noti, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(s_noti, fw_theme_color_warning(), 0);
    lv_obj_align(s_noti, LV_ALIGN_RIGHT_MID, -72, 0);
    lv_obj_add_flag(s_noti, LV_OBJ_FLAG_HIDDEN);

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh_time();

    lvgl_port_unlock();

    svc_event_bus_subscribe(SVC_EVENT_TIME_SYNCED, evt_time, NULL);
    svc_event_bus_subscribe(SVC_EVENT_TIME_CHANGED, evt_time, NULL);
    svc_event_bus_subscribe(SVC_EVENT_TIMEZONE_CHANGED, evt_time, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_AUDIO_PLAYBACK_STARTED, evt_audio, NULL);
    svc_event_bus_subscribe(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, evt_audio, NULL);
    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_POSTED, evt_noti, NULL);
    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_DISMISSED, evt_noti, NULL);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}
