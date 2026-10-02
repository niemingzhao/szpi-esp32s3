/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Status Bar 实现
 *
 * 全局状态栏（挂 lv_layer_top()），不随 App 屏幕切换消失。它同时承担全局导航：
 *   [返回] [主页]      时间      [Wi-Fi] [蓝牙] [声音] [录音] [摄像头] [TF 卡]
 * 右侧图标全部常显（不隐藏），用自绘的 20x20 图标（fw_status_icons.h）+ image_recolor
 * 染色：启用 / 触发时强调色高亮，否则置灰。时间居中于左按钮组与右图标组之间
 * （屏幕偏左），事件回调运行在 svc_event_bus 的 dispatcher 任务，访问 LVGL 前必须加锁。
 */

#include "fw_common.h"
#include "fw_status_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.statusbar";

static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_time = NULL;
static lv_obj_t *s_wifi = NULL;
static lv_obj_t *s_bt = NULL;
static lv_obj_t *s_sound = NULL;
static lv_obj_t *s_record = NULL;
static lv_obj_t *s_camera = NULL;
static lv_obj_t *s_sd = NULL;
static lv_timer_t *s_timer = NULL;
static bool s_wifi_on = false;
static bool s_bt_on = false;
static bool s_sound_on = false;
static bool s_record_on = false;
static bool s_camera_on = false;
static bool s_sd_on = false;

/* 图标高亮 / 置灰（统一的强调色 / 置灰色）。颜色没变就不动样式，避免每秒无谓重绘 */
static void icon_set_on(lv_obj_t *icon, bool on)
{
    if (icon == NULL) return;

    const lv_color_t want = on ? fw_theme_color_accent() : fw_theme_color_text_disabled();
    if (lv_color_eq(lv_obj_get_style_image_recolor(icon, 0), want)) return;

    lv_obj_set_style_image_recolor(icon, want, 0);
}

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

/* 每秒按真实状态把所有图标同步一次：事件是主路径（即时），这里是兜底，
 * 漏掉某个事件或订阅前状态就已改变时不会让图标停在旧状态。
 * 由 LVGL 定时器调用，已经在 LVGL 任务里，不必再加锁。
 * TF 卡不在这里轮询（查容量要访问文件系统，代价大），仍由挂载 / 卸载事件维护。 */
static void refresh_icons(void)
{
    svc_net_status_t net;
    if (svc_net_get_status(&net) == ESP_OK) {
        s_wifi_on = net.wifi_connected;
        icon_set_on(s_wifi, s_wifi_on);
    }

    const svc_bt_state_t bt = svc_bt_get_state();
    s_bt_on = (bt == SVC_BT_STATE_ADVERTISING || bt == SVC_BT_STATE_CONNECTED);
    icon_set_on(s_bt, s_bt_on);

    const svc_audio_state_t au = svc_audio_get_state();
    s_sound_on = (au == SVC_AUDIO_STATE_PLAYING);
    s_record_on = (au == SVC_AUDIO_STATE_RECORDING);
    icon_set_on(s_sound, s_sound_on);
    icon_set_on(s_record, s_record_on);

    s_camera_on = svc_camera_is_open();
    icon_set_on(s_camera, s_camera_on);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_time();
    refresh_icons();
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

static lv_obj_t *make_bar_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb,
                              lv_align_t align, lv_coord_t x, lv_coord_t w)
{
    lv_obj_t *btn = lv_button_create(parent);
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

/* 状态图标：自绘资源是白色 + alpha，靠 image_recolor 染色（不透明覆盖） */
static lv_obj_t *make_bar_icon(lv_obj_t *parent, const lv_image_dsc_t *src, lv_align_t align, lv_coord_t x)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, fw_theme_color_text_disabled(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_align(img, align, x, 0);
    return img;
}

/* ------------------------------- 状态 setter ------------------------------- */

esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected)
{
    (void)rssi;
    lvgl_port_lock(0);
    s_wifi_on = connected;
    icon_set_on(s_wifi, connected);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_statusbar_set_sound_playing(bool on)
{
    lvgl_port_lock(0);
    s_sound_on = on;
    icon_set_on(s_sound, on);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_statusbar_set_bluetooth(bool on)
{
    lvgl_port_lock(0);
    s_bt_on = on;
    icon_set_on(s_bt, on);
    lvgl_port_unlock();
    return ESP_OK;
}

/* -------------------------------- 事件回调 -------------------------------- */

/* 录音：录音中高亮（与其它图标同一强调色），其余时候置灰（常显，不隐藏） */
static void set_recording(bool on)
{
    lvgl_port_lock(0);
    s_record_on = on;
    icon_set_on(s_record, on);
    lvgl_port_unlock();
}

static void set_sd(bool mounted)
{
    lvgl_port_lock(0);
    s_sd_on = mounted;
    icon_set_on(s_sd, mounted);
    lvgl_port_unlock();
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
    fw_statusbar_set_sound_playing(evt->id == SVC_EVENT_AUDIO_PLAYBACK_STARTED);
}

/* 蓝牙图标：广播中或已连接时高亮；协议栈就绪但没广播（READY）置灰 */
static void evt_bt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    const svc_bt_state_t st = svc_bt_get_state();
    fw_statusbar_set_bluetooth(st == SVC_BT_STATE_ADVERTISING || st == SVC_BT_STATE_CONNECTED);
}

/* 摄像头图标：打开 / 关闭事件即时更新（1 s 轮询作为兜底） */
static void evt_camera(const svc_event_t *evt, void *user)
{
    (void)user;

    bool opened = svc_camera_is_open();
    if (evt->data != NULL && evt->data_len >= sizeof(bool)) {
        opened = *(const bool *)evt->data;
    }

    lvgl_port_lock(0);
    s_camera_on = opened;
    icon_set_on(s_camera, opened);
    lvgl_port_unlock();
}

/* 录音图标：录音开始高亮，结束置灰 */
static void evt_record(const svc_event_t *evt, void *user)
{
    (void)user;
    set_recording(evt->id == SVC_EVENT_AUDIO_RECORD_STARTED);
}

/* TF 卡图标：挂载点亮，卸载置灰 */
static void evt_sd(const svc_event_t *evt, void *user)
{
    (void)user;
    set_sd(evt->id == SVC_EVENT_SD_MOUNTED);
}

/* ---------------------------------- 初始化 ---------------------------------- */

static void statusbar_build(void)
{
    s_bar = lv_obj_create(lv_layer_top());
    lv_obj_set_width(s_bar, lv_pct(100));
    lv_obj_set_height(s_bar, FW_STATUSBAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_scrollable(s_bar, false);
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

    /* 中：时间，居中于左按钮组与右图标组之间的可用区域（中点约 x=128） */
    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, -32, 0);

    /* 右：状态图标，常显。按需求顺序从左到右：Wi-Fi、蓝牙、声音、录音、摄像头、TF 卡
     * （20x20，用 RIGHT_MID 对齐，x 越小越靠左，间距 24 px） */
    s_wifi = make_bar_icon(s_bar, &icon_status_wifi, LV_ALIGN_RIGHT_MID, -128);
    s_bt = make_bar_icon(s_bar, &icon_status_bt, LV_ALIGN_RIGHT_MID, -104);
    s_sound = make_bar_icon(s_bar, &icon_status_sound, LV_ALIGN_RIGHT_MID, -80);
    s_record = make_bar_icon(s_bar, &icon_status_record, LV_ALIGN_RIGHT_MID, -56);
    s_camera = make_bar_icon(s_bar, &icon_status_camera, LV_ALIGN_RIGHT_MID, -32);
    s_sd = make_bar_icon(s_bar, &icon_status_sd, LV_ALIGN_RIGHT_MID, -8);

    /* 重建时按真实状态重新套一遍（事件在订阅之前发布过也不会漏） */
    periph_storage_info_t sd_info;
    s_sd_on = (svc_storage_get_info(PERIPH_STORAGE_TF_CARD, &sd_info) == ESP_OK);
    set_sd(s_sd_on);
    refresh_icons();
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
    svc_event_bus_subscribe(SVC_EVENT_AUDIO_RECORD_STARTED, evt_record, NULL);
    svc_event_bus_subscribe(SVC_EVENT_AUDIO_RECORD_FINISHED, evt_record, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, evt_bt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_CAMERA_STATE_CHANGED, evt_camera, NULL);
    svc_event_bus_subscribe(SVC_EVENT_SD_MOUNTED, evt_sd, NULL);
    svc_event_bus_subscribe(SVC_EVENT_SD_UNMOUNTED, evt_sd, NULL);

    ESP_LOGI(TAG, "initialized (%d px)", FW_STATUSBAR_H);
    return ESP_OK;
}

/* 换主题时重建控件（订阅与定时器保持不变） */
esp_err_t fw_statusbar_rebuild(void)
{
    lvgl_port_lock(0);

    if (s_bar != NULL) {
        lv_obj_delete(s_bar);
        s_bar = NULL;
        s_time = NULL;
        s_wifi = NULL;
        s_bt = NULL;
        s_sound = NULL;
        s_record = NULL;
        s_camera = NULL;
        s_sd = NULL;
    }

    statusbar_build();

    lvgl_port_unlock();
    return ESP_OK;
}
