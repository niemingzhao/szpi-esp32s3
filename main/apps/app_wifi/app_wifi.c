/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Wi-Fi（APP-WIFI 网络）
 *
 * 状态 + 扫描列表 + 连接（列表点选：开放网络直连，加密网络弹密码输入页）
 * + 配网热点 + SmartConfig 配网 + 忘记网络。
 *
 * 扫描会阻塞数秒，放在一次性任务里跑，完成后用 lv_async_call 回到 LVGL 任务刷新 ——
 * 在 LVGL 事件回调里同步扫描会把 LVGL 任务卡住，同核的低优先级任务会被饿死。
 * 连接状态变化经 SVC_EVENT_WIFI_CONNECTED / SVC_EVENT_WIFI_DISCONNECTED 通知。
 */

#include "app_wifi.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.wifi";

#define WIFI_SCAN_MAX       16
#define WIFI_SCAN_TIMEOUT   8000
#define WIFI_SCAN_STACK     4096
#define CONN_KB_HEIGHT      130

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_state_row = NULL;
static lv_obj_t *s_saved_row = NULL;
static lv_obj_t *s_prov_row = NULL;
static lv_obj_t *s_sc_row = NULL;
static lv_obj_t *s_list = NULL;

/* SmartConfig 配网状态。svc_net 没有公开的 is_active 查询，且智能配网成功后会自行停止，
 * 所以这里本地记录，并在 Wi-Fi 连上后收回（见 refresh_status）。 */
static bool s_sc_on = false;

/* 密码输入浮层 */
static lv_obj_t *s_conn = NULL;
static lv_obj_t *s_conn_ta = NULL;
static char s_conn_ssid[33] = { 0 };

static svc_net_wifi_ap_t s_aps[WIFI_SCAN_MAX];
static size_t s_ap_count = 0;
static bool s_scanning = false;

/* ---------------------------------- 状态 ---------------------------------- */

static void refresh_status(void)
{
    if (s_state_row == NULL) return;

    svc_net_status_t st;
    memset(&st, 0, sizeof(st));
    if (svc_net_get_status(&st) != ESP_OK) return;

    char buf[48];
    if (st.wifi_connected) {
        snprintf(buf, sizeof(buf), "%.20s  %.15s", st.wifi_ssid, st.ip_addr);
    } else {
        snprintf(buf, sizeof(buf), "未连接");
    }
    fw_ui_row_btn_value(s_state_row, buf);

    char saved[33] = { 0 };
    if (svc_net_wifi_get_saved_ssid(saved, sizeof(saved)) == ESP_OK) {
        fw_ui_row_btn_value(s_saved_row, saved);
    } else {
        fw_ui_row_btn_value(s_saved_row, "无");
    }

    fw_ui_row_btn_value(s_prov_row, svc_net_prov_is_active() ? "进行中" : "关");

    /* SmartConfig 成功后服务会自行停止并连接：连上就把本地状态收回，避免状态行一直显示"进行中" */
    if (s_sc_on && st.wifi_connected) {
        s_sc_on = false;
    }
    fw_ui_row_btn_value(s_sc_row, s_sc_on ? "进行中" : "关");
}

/* ---------------------------------- 连接 ---------------------------------- */

static void connect_with(const char *ssid, const char *password)
{
    svc_net_wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    if (password != NULL) {
        strncpy(creds.password, password, sizeof(creds.password) - 1);
    }

    if (svc_net_wifi_connect(&creds) != ESP_OK) {
        fw_ui_toast("连接失败", 2000);
        return;
    }
    fw_ui_toast("正在连接…", 2000);
}

static void conn_close(void)
{
    if (s_conn != NULL) {
        lv_obj_delete(s_conn);
        s_conn = NULL;
        s_conn_ta = NULL;
    }
    s_conn_ssid[0] = '\0';
}

static void kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        const char *pw = (s_conn_ta != NULL) ? lv_textarea_get_text(s_conn_ta) : "";
        connect_with(s_conn_ssid, pw);
        conn_close();
    } else if (code == LV_EVENT_CANCEL) {
        conn_close();
    }
}

static void conn_open(const char *ssid)
{
    if (s_conn != NULL) return;

    strncpy(s_conn_ssid, ssid, sizeof(s_conn_ssid) - 1);

    s_conn = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_conn);
    lv_obj_set_size(s_conn, lv_pct(100), lv_pct(100));
    lv_obj_center(s_conn);
    lv_obj_set_style_bg_color(s_conn, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_conn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_conn, 8, 0);
    lv_obj_set_scrollable(s_conn, false);

    char title[48];
    snprintf(title, sizeof(title), "连接到 %s", s_conn_ssid);

    lv_obj_t *lb = lv_label_create(s_conn);
    lv_label_set_text(lb, title);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 0, 0);

    s_conn_ta = lv_textarea_create(s_conn);
    lv_textarea_set_one_line(s_conn_ta, true);
    lv_textarea_set_password_mode(s_conn_ta, true);
    lv_textarea_set_placeholder_text(s_conn_ta, "密码");
    lv_obj_set_size(s_conn_ta, lv_pct(100), 36);
    lv_obj_align(s_conn_ta, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_font(s_conn_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_conn_ta, fw_theme_color_text_primary(), 0);

    lv_obj_t *kb = lv_keyboard_create(s_conn);
    lv_obj_set_size(kb, lv_pct(100), CONN_KB_HEIGHT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_conn_ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------------------------------- 列表 ---------------------------------- */

static void ap_cb(lv_event_t *e)
{
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_ap_count) return;

    const svc_net_wifi_ap_t *ap = &s_aps[idx - 1];
    if (ap->auth_mode == 0) {
        connect_with(ap->ssid, NULL);       /* 开放网络直接连 */
    } else {
        conn_open(ap->ssid);
    }
}

static void fill_scan_list(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    if (s_scanning) {
        fw_ui_list_add(s_list, "正在扫描…", NULL, NULL);
        return;
    }
    if (s_ap_count == 0) {
        fw_ui_list_add(s_list, "未发现网络", NULL, NULL);
        return;
    }

    char line[64];
    for (size_t i = 0; i < s_ap_count; i++) {
        snprintf(line, sizeof(line), "%s  %d dBm", s_aps[i].ssid, (int)s_aps[i].rssi);
        /* user_data 传下标 + 1（0 表示无效） */
        fw_ui_list_add(s_list, line, ap_cb, (void *)(uintptr_t)(i + 1));
    }
}

/* ---------------------------------- 扫描 ---------------------------------- */

static void scan_done_async(void *user)
{
    (void)user;

    fill_scan_list();

    char msg[32];
    snprintf(msg, sizeof(msg), "发现 %u 个网络", (unsigned)s_ap_count);
    fw_ui_toast(msg, 2000);
}

static void scan_task(void *arg)
{
    (void)arg;

    if (svc_net_wifi_scan(s_aps, WIFI_SCAN_MAX, &s_ap_count, WIFI_SCAN_TIMEOUT) != ESP_OK) {
        s_ap_count = 0;
    }
    s_scanning = false;

    if (lvgl_port_lock(0)) {
        lv_async_call(scan_done_async, NULL);
        lvgl_port_unlock();
    }

    vTaskDelete(NULL);
}

static void scan_cb(lv_event_t *e)
{
    (void)e;

    if (s_scanning) return;

    s_scanning = true;
    fill_scan_list();                       /* 先显示"正在扫描…" */

    if (xTaskCreate(scan_task, "wifi_scan", WIFI_SCAN_STACK, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        fill_scan_list();
        fw_ui_toast("扫描失败", 2000);
    }
}

/* -------------------------------- 其他操作 -------------------------------- */

static void disconnect_cb(lv_event_t *e)
{
    (void)e;
    svc_net_wifi_disconnect();
    refresh_status();
}

static void prov_cb(lv_event_t *e)
{
    (void)e;

    if (svc_net_prov_is_active()) {
        svc_net_prov_stop();
    } else if (svc_net_prov_start(NULL, NULL) != ESP_OK) {
        fw_ui_toast("配网启动失败", 2000);
    } else {
        fw_ui_toast("浏览器访问 192.168.4.1", 4000);
    }
    refresh_status();
}

/* SmartConfig：手机用 ESP 配网 App 把 Wi-Fi 信息广播给设备（区别于上面的 AP + 网页配网） */
static void smartconfig_cb(lv_event_t *e)
{
    (void)e;

    if (s_sc_on) {
        svc_net_smartconfig_stop();
        s_sc_on = false;
    } else if (svc_net_smartconfig_start() != ESP_OK) {
        fw_ui_toast("SmartConfig 启动失败", 2000);
    } else {
        s_sc_on = true;
        fw_ui_toast("手机用 ESP 配网 App 发送 Wi-Fi 信息", 4000);
    }
    refresh_status();
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    svc_net_wifi_forget();
    refresh_status();
    fw_ui_toast("已清除凭据", 2000);
}

static void on_wifi_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    refresh_status();
    lvgl_port_unlock();
}

/* ---------------------------------- 生命周期 ---------------------------------- */

static void *wifi_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_state_row = fw_ui_row_btn(body, LV_SYMBOL_WIFI, "状态", NULL, NULL);
    s_saved_row = fw_ui_row_btn(body, LV_SYMBOL_SAVE, "已保存", NULL, NULL);
    s_prov_row = fw_ui_row_btn(body, LV_SYMBOL_SETTINGS, "配网热点", prov_cb, NULL);
    s_sc_row = fw_ui_row_btn(body, LV_SYMBOL_UPLOAD, "SmartConfig 配网", smartconfig_cb, NULL);

    fw_ui_row_btn(body, LV_SYMBOL_REFRESH, "扫描网络", scan_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_CLOSE, "断开", disconnect_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_TRASH, "忘记网络", forget_cb, NULL);

    s_list = fw_ui_list(body, "可用网络");

    refresh_status();
    fill_scan_list();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void wifi_on_start(void *ctx)
{
    (void)ctx;
    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, on_wifi_evt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, on_wifi_evt, NULL);

    lvgl_port_lock(0);
    refresh_status();
    lvgl_port_unlock();
}

static void wifi_on_pause(void *ctx)
{
    (void)ctx;
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_CONNECTED, on_wifi_evt);
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_DISCONNECTED, on_wifi_evt);
}

static void wifi_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_conn = NULL;                          /* 随根屏一起删掉，避免悬空 */
    s_conn_ta = NULL;
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_state_row = NULL;
    s_saved_row = NULL;
    s_prov_row = NULL;
    s_sc_row = NULL;
    s_list = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_wifi_desc = {
    .name = "Wi-Fi",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_WIFI,
    .on_create = wifi_on_create,
    .on_start = wifi_on_start,
    .on_pause = wifi_on_pause,
    .on_destroy = wifi_on_destroy,
};
