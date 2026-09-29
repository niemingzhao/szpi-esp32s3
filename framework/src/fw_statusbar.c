/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Status Bar 实现
 *
 * 全局状态栏（挂 lv_layer_top()），不随 App 屏幕切换消失。它同时承担全局导航：
 *   [返回] [主页]        时间        [Wi-Fi] [音乐] [通知中心] [控制中心]
 * 事件回调运行在 svc_event_bus 的 dispatcher 任务，访问 LVGL 前必须加锁。
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.statusbar";

static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_time = NULL;
static lv_obj_t *s_wifi = NULL;
static lv_obj_t *s_music = NULL;
static lv_obj_t *s_noti_icon = NULL;
static lv_timer_t *s_timer = NULL;
static bool s_wifi_on = false;
static bool s_music_on = false;
static bool s_bt_on = false;
static lv_obj_t *s_bt = NULL;
static uint8_t s_bright_pct = 0;
static lv_obj_t *s_bright = NULL;

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

/* ---------------------------------- 导航 ---------------------------------- */

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

static void nav_noti_cb(lv_event_t *e)
{
    (void)e;

    if (fw_notification_is_visible()) {
        fw_notification_hide();
    } else {
        fw_control_center_hide();   /* 同一时刻只保留一个浮层 */
        fw_notification_show();
    }
}

static void nav_ctrl_cb(lv_event_t *e)
{
    (void)e;

    if (fw_control_center_is_visible()) {
        fw_control_center_hide();
    } else {
        fw_notification_hide();
        fw_control_center_show();
    }
}

static lv_obj_t *make_bar_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb,
                              lv_align_t align, lv_coord_t x, lv_coord_t w)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, FW_STATUSBAR_H - 8);
    lv_obj_align(btn, align, x, 0);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 8);      /* 视觉不变，触摸区域向四周放大 8 px */
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *make_bar_icon(lv_obj_t *parent, const char *symbol, lv_align_t align, lv_coord_t x)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_disabled(), 0);
    lv_obj_align(label, align, x, 0);
    return label;
}

/* ------------------------------- 状态 setter ------------------------------- */

esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected)
{
    (void)rssi;
    lvgl_port_lock(0);
    s_wifi_on = connected;
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
    s_music_on = on;
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
    s_bt_on = on;
    if (s_bt != NULL) {
        lv_obj_set_style_text_color(s_bt,
                                    on ? fw_theme_color_text_primary()
                                       : fw_theme_color_text_disabled(),
                                    0);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_statusbar_set_brightness(uint8_t percent)
{
    lvgl_port_lock(0);
    s_bright_pct = percent;
    if (s_bright != NULL) {
        lv_color_t c;
        if (percent == 0) {
            c = fw_theme_color_text_disabled();      /* 背光关闭 */
        } else if (percent < 50) {
            c = fw_theme_color_text_secondary();
        } else {
            c = fw_theme_color_text_primary();
        }
        lv_obj_set_style_text_color(s_bright, c, 0);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

/* -------------------------------- 事件回调 -------------------------------- */

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

/* 蓝牙图标跟随 BLE 状态（协议栈在 Services 初始化早期启动，非 OFF 即点亮） */
static void evt_bt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    fw_statusbar_set_bluetooth(svc_bt_get_state() != SVC_BT_STATE_OFF);
}

/* 亮度图标跟随背光（Settings / 控制中心改动都会走到这里） */
static void evt_brightness(const svc_event_t *evt, void *user)
{
    (void)user;
    if (evt->data == NULL || evt->data_len < sizeof(uint8_t)) return;
    fw_statusbar_set_brightness(*(const uint8_t *)evt->data);
}

/* 有通知时把铃铛点亮（常驻入口，不隐藏） */
static void evt_noti(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    if (s_noti_icon != NULL) {
        lv_obj_set_style_text_color(s_noti_icon,
                                    svc_notification_get_count() > 0 ? fw_theme_color_warning()
                                                                     : fw_theme_color_text_primary(),
                                    0);
    }
    lvgl_port_unlock();
}

/* ---------------------------------- 初始化 ---------------------------------- */

static void statusbar_build(void)
{
    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(NULL), FW_STATUSBAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_bar, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    /* 底部 1 px 线：浅色主题下把状态栏和页面分开 */
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(s_bar, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_text_font(s_bar, fw_asset_font_14(), 0);

    /* 左：返回 / 主页 */
    make_bar_btn(s_bar, LV_SYMBOL_LEFT, nav_back_cb, LV_ALIGN_LEFT_MID, 8, 32);
    make_bar_btn(s_bar, LV_SYMBOL_HOME, nav_home_cb, LV_ALIGN_LEFT_MID, 48, 32);

    /* 中：时间 */
    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 0);

    /* 右：状态图标 + 通知中心 / 控制中心 */
    make_bar_btn(s_bar, LV_SYMBOL_SETTINGS, nav_ctrl_cb, LV_ALIGN_RIGHT_MID, -8, 32);
    lv_obj_t *noti_btn = make_bar_btn(s_bar, LV_SYMBOL_BELL, nav_noti_cb, LV_ALIGN_RIGHT_MID, -44, 32);
    s_noti_icon = lv_obj_get_child(noti_btn, 0);

    /* 状态图标（从右往左依次靠近；间距 18 px，最左侧的亮度图标要给中间时钟留位） */
    s_wifi = make_bar_icon(s_bar, LV_SYMBOL_WIFI, LV_ALIGN_RIGHT_MID, -76);
    s_music = make_bar_icon(s_bar, LV_SYMBOL_AUDIO, LV_ALIGN_RIGHT_MID, -94);
    lv_obj_set_style_text_color(s_music, fw_theme_color_accent(), 0);
    s_bt = make_bar_icon(s_bar, LV_SYMBOL_BLUETOOTH, LV_ALIGN_RIGHT_MID, -112);
    s_bright = make_bar_icon(s_bar, LV_SYMBOL_TINT, LV_ALIGN_RIGHT_MID, -132);

    /* 重建时把当前状态重新套上（BLE 在 Services 早期就启动了，这里同步一次真实状态） */
    s_bt_on = (svc_bt_get_state() != SVC_BT_STATE_OFF);
    fw_statusbar_set_wifi(0, s_wifi_on);
    fw_statusbar_set_music_playing(s_music_on);
    fw_statusbar_set_bluetooth(s_bt_on);
    fw_statusbar_set_brightness(s_bright_pct);
    evt_noti(NULL, NULL);
    refresh_time();
}

esp_err_t fw_statusbar_init(void)
{
    lvgl_port_lock(0);
    statusbar_build();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    lvgl_port_unlock();

    svc_event_bus_subscribe(SVC_EVENT_TIME_SYNCED, evt_time, NULL);
    svc_event_bus_subscribe(SVC_EVENT_TIME_CHANGED, evt_time, NULL);
    svc_event_bus_subscribe(SVC_EVENT_TIMEZONE_CHANGED, evt_time, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_AUDIO_PLAYBACK_STARTED, evt_audio, NULL);
    svc_event_bus_subscribe(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, evt_audio, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, evt_bt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BRIGHTNESS_CHANGED, evt_brightness, NULL);
    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_POSTED, evt_noti, NULL);
    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_DISMISSED, evt_noti, NULL);

    ESP_LOGI(TAG, "initialized (%d px)", FW_STATUSBAR_H);
    return ESP_OK;
}

/* 换主题时重建控件（订阅与定时器保持不变） */
esp_err_t fw_statusbar_rebuild(void)
{
    lvgl_port_lock(0);

    if (s_bar != NULL) {
        lv_obj_del(s_bar);
        s_bar = NULL;
        s_time = NULL;
        s_wifi = NULL;
        s_music = NULL;
        s_bt = NULL;
        s_bright = NULL;
        s_noti_icon = NULL;
    }

    statusbar_build();

    lvgl_port_unlock();
    return ESP_OK;
}
