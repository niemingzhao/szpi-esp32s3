/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Wi-Fi（APP-WIFI 网络）
 *
 * 单屏布局（页面风格见 docs/03-design/02-ui-system.md 12.6）：
 *   主页：头部（连接状态 + 扫描 / 设置两个图标按钮）+ 当前网络卡（SSID / IP / 信号）
 *         + 可用网络列表（可滚动，点一下连接；加密网络先输密码）
 *        未连接时隐藏状态卡、列表撑满（不留 "--" 之类的占位，和天气不一样）
 *   设置页：断开/重连 / 配网热点 / SmartConfig 配网 / 忘记网络（二次确认）
 *   密码浮层：标题行带"连接"按钮，输入行带"显示密码"选框，返回键关浮层
 *
 * 扫描会阻塞数秒，放在一次性任务里跑，完成后用 lv_async_call 回到 LVGL 任务刷新 ——
 * 在 LVGL 事件回调里同步扫描会把 LVGL 任务卡住，同核的低优先级任务会被饿死。
 * 连接状态变化经 SVC_EVENT_WIFI_CONNECTED / SVC_EVENT_WIFI_DISCONNECTED 通知。
 */

#include "app_wifi.h"
#include "fw_common.h"
#include "fw_home_icons.h"
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

#define HEADER_H            30
#define CARD_H              52
#define LIST_BOX_H          92
#define ROW_H               32
#define SET_ROW_H           40
#define KB_H                120
#define OVERLAY_TOP         30

enum { PAGE_MAIN = 0, PAGE_SET, PAGE_COUNT };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page[PAGE_COUNT] = { 0 };
static int s_page_cur = PAGE_MAIN;

static lv_obj_t *s_state_lb = NULL;      /* 头部：连接状态 */
static lv_obj_t *s_card = NULL;          /* 当前网络卡（未连接时整块隐藏） */
static lv_obj_t *s_ssid_lb = NULL;
static lv_obj_t *s_ip_lb = NULL;
static lv_obj_t *s_rssi_lb = NULL;
static lv_obj_t *s_list = NULL;          /* 可用网络（可滚动） */

static lv_obj_t *s_disc_row = NULL;      /* 设置页各行（数值用 fw_ui_row_btn_value 更新） */
static lv_obj_t *s_prov_row = NULL;
static lv_obj_t *s_sc_row = NULL;
static lv_obj_t *s_saved_row = NULL;

/* 密码浮层 */
static lv_obj_t *s_conn = NULL;
static lv_obj_t *s_conn_ta = NULL;
static lv_obj_t *s_conn_kb = NULL;
static char s_conn_ssid[33] = { 0 };

/* SmartConfig 状态。svc_net 没有公开的 is_active 查询，且智能配网成功后会自行停止，
 * 所以这里本地记录，并在 Wi-Fi 连上后收回（见 refresh_status）。 */
static bool s_sc_on = false;

static svc_net_wifi_ap_t s_aps[WIFI_SCAN_MAX];
static size_t s_ap_count = 0;
static bool s_scanning = false;
static bool s_foreground = false;

static void refresh_status(void);
static void fill_scan_list(void);
static void page_show(int page);

/* ---------------------------------- 状态 ---------------------------------- */

/* 改整行按钮左侧的文本（fw_ui_row_btn 的子对象固定是：图标 0、文本 1、数值 2） */
static void row_set_label(lv_obj_t *row, const char *text)
{
    if (row == NULL || lv_obj_get_child_cnt(row) < 2) return;

    lv_obj_t *lb = lv_obj_get_child(row, 1);
    if (lb != NULL && lv_obj_get_user_data(lb) == NULL) {
        lv_label_set_text(lb, text);
    }
}

static void refresh_status(void)
{
    svc_net_status_t st;
    memset(&st, 0, sizeof(st));
    const bool ok = (svc_net_get_status(&st) == ESP_OK);
    const bool conn = ok && st.wifi_connected;

    char buf[48];

    if (s_state_lb != NULL) {
        snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_WIFI, conn ? "已连接" : "未连接");
        lv_label_set_text(s_state_lb, buf);
    }

    /* 未连接时整块隐藏状态卡，不留占位（列表会撑满剩余空间） */
    if (s_card != NULL) {
        lv_obj_set_hidden(s_card, !conn);
    }
    if (s_ssid_lb != NULL) {
        lv_label_set_text(s_ssid_lb, conn ? st.wifi_ssid : "");
    }
    if (s_rssi_lb != NULL) {
        if (conn) snprintf(buf, sizeof(buf), "%d dBm", (int)st.rssi);
        else buf[0] = '\0';
        lv_label_set_text(s_rssi_lb, buf);
    }
    if (s_ip_lb != NULL) {
        if (conn) snprintf(buf, sizeof(buf), "IP %s", st.ip_addr);
        else buf[0] = '\0';
        lv_label_set_text(s_ip_lb, buf);
    }

    char saved[33] = { 0 };
    const bool has_saved = (svc_net_wifi_get_saved_ssid(saved, sizeof(saved)) == ESP_OK);

    /* 断开 / 重连：连上时是"断开"，断开时按已保存凭据重连（不需要再输密码） */
    if (s_disc_row != NULL) {
        row_set_label(s_disc_row, conn ? "断开" : "重连");
        fw_ui_row_btn_value(s_disc_row, "");
    }
    if (s_prov_row != NULL) {
        fw_ui_row_btn_value(s_prov_row, svc_net_prov_is_active() ? "开" : "关");
    }
    if (s_sc_on && conn) {
        s_sc_on = false;
    }
    if (s_sc_row != NULL) {
        fw_ui_row_btn_value(s_sc_row, s_sc_on ? "开" : "关");
    }
    if (s_saved_row != NULL) {
        fw_ui_row_btn_value(s_saved_row, has_saved ? saved : "");
    }

    fill_scan_list();        /* 列表里把当前连接的那个标出来 */
}

/* ---------------------------------- 连接 ---------------------------------- */

static void connect_with(const char *ssid, const char *password)
{
    svc_net_wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    strlcpy(creds.ssid, ssid, sizeof(creds.ssid));
    if (password != NULL) {
        strlcpy(creds.password, password, sizeof(creds.password));
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
        s_conn_kb = NULL;
    }
    s_conn_ssid[0] = '\0';
}

static void conn_kb_hide(void)
{
    if (s_conn_kb != NULL) {
        lv_obj_set_hidden(s_conn_kb, true);
        lv_keyboard_set_textarea(s_conn_kb, NULL);   /* 解绑，避免焦点残留 */
    }
    if (s_conn_ta != NULL) {
        lv_obj_clear_state(s_conn_ta, LV_STATE_FOCUSED);
    }
}

/* 提交密码：先把 SSID 拷出来再关浮层（conn_close 会清空 s_conn_ssid） */
static void conn_submit(void)
{
    if (s_conn == NULL) return;

    char pw[65] = { 0 };
    if (s_conn_ta != NULL) {
        snprintf(pw, sizeof(pw), "%s", lv_textarea_get_text(s_conn_ta));
    }

    char ssid[33];
    strlcpy(ssid, s_conn_ssid, sizeof(ssid));

    conn_close();
    connect_with(ssid, pw);
}

static void conn_ok_cb(lv_event_t *e)
{
    (void)e;
    conn_submit();
}

static void conn_ta_cb(lv_event_t *e)
{
    (void)e;
    /* 用 CLICKED：输入框可能已经是焦点态，FOCUSED 不会再来 */
    if (s_conn_kb != NULL) {
        lv_keyboard_set_textarea(s_conn_kb, s_conn_ta);
        lv_obj_set_hidden(s_conn_kb, false);
        lv_obj_move_foreground(s_conn_kb);
    }
}

/* 点浮层空白处（不是输入框 / 键盘 / 按钮）只收起键盘，不关浮层 */
static void conn_click_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_conn) return;
    conn_kb_hide();
}

static void conn_show_pw_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    const bool on = lv_obj_has_state(cb, LV_STATE_CHECKED);
    if (s_conn_ta != NULL) {
        lv_textarea_set_password_mode(s_conn_ta, !on);
    }
}

static void conn_kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        conn_submit();
    } else if (code == LV_EVENT_CANCEL) {
        conn_kb_hide();
    }
}

static void conn_open(const char *ssid)
{
    if (s_conn != NULL || s_root == NULL) return;

    strlcpy(s_conn_ssid, ssid, sizeof(s_conn_ssid));

    s_conn = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_conn);
    lv_obj_set_size(s_conn, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_conn, 0, 0);
    lv_obj_set_style_bg_color(s_conn, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_conn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_conn, 8, 0);
    lv_obj_set_style_pad_top(s_conn, OVERLAY_TOP, 0);
    lv_obj_set_scrollable(s_conn, false);
    lv_obj_add_event_cb(s_conn, conn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 标题行：标题（撑满）+ 连接 */
    lv_obj_t *title_row = lv_obj_create(s_conn);
    lv_obj_set_size(title_row, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_pad_column(title_row, 6, 0);
    lv_obj_set_scrollable(title_row, false);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(title_row, LV_ALIGN_TOP_MID, 0, 0);

    char title[48];
    snprintf(title, sizeof(title), "连接到 %.24s", s_conn_ssid);

    lv_obj_t *lb = lv_label_create(title_row);
    lv_obj_set_flex_grow(lb, 1);
    lv_label_set_text(lb, title);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);

    lv_obj_t *ok = lv_button_create(title_row);
    lv_obj_set_size(ok, 56, 28);
    lv_obj_set_style_bg_color(ok, fw_theme_color_accent(), 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_style_radius(ok, 6, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_set_style_pad_all(ok, 0, 0);
    lv_obj_add_event_cb(ok, conn_ok_cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *ok_lb = lv_label_create(ok);
    lv_label_set_text(ok_lb, "连接");
    lv_obj_set_style_text_font(ok_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(ok_lb, lv_color_white(), 0);
    lv_obj_center(ok_lb);

    /* 输入行：密码框（撑满）+ 显示密码 */
    lv_obj_t *in_row = lv_obj_create(s_conn);
    lv_obj_set_size(in_row, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(in_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(in_row, 0, 0);
    lv_obj_set_style_pad_all(in_row, 0, 0);
    lv_obj_set_style_pad_column(in_row, 8, 0);
    lv_obj_set_scrollable(in_row, false);
    lv_obj_set_flex_flow(in_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(in_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(in_row, LV_ALIGN_TOP_MID, 0, 34);

    s_conn_ta = lv_textarea_create(in_row);
    lv_textarea_set_one_line(s_conn_ta, true);
    lv_textarea_set_password_mode(s_conn_ta, true);
    lv_textarea_set_placeholder_text(s_conn_ta, "密码");
    lv_obj_set_height(s_conn_ta, 36);
    lv_obj_set_flex_grow(s_conn_ta, 1);
    lv_obj_set_style_text_font(s_conn_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_conn_ta, fw_theme_color_text_primary(), 0);
    lv_obj_add_event_cb(s_conn_ta, conn_ta_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *show = lv_checkbox_create(in_row);
    lv_checkbox_set_text(show, "显示密码");
    lv_obj_set_style_text_font(show, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(show, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_bg_color(show, fw_theme_color_bg_card(), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(show, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(show, fw_theme_color_border(), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(show, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(show, fw_theme_color_accent(),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(show, fw_theme_color_accent(),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(show, conn_show_pw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 键盘：先藏起来，点输入框再弹出（弹出时不会压住标题行与输入行） */
    s_conn_kb = lv_keyboard_create(s_conn);
    lv_obj_set_size(s_conn_kb, lv_pct(100), KB_H);
    lv_obj_align(s_conn_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_conn_kb, conn_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_conn_kb, conn_kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_set_hidden(s_conn_kb, true);
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

static lv_obj_t *make_ap_row(lv_obj_t *parent, size_t idx)
{
    const svc_net_wifi_ap_t *ap = &s_aps[idx];

    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_color(row, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_event_cb(row, ap_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)(idx + 1));

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, ap->ssid);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(64));
    lv_obj_set_style_text_font(name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(name, fw_theme_color_text_primary(), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);

    char buf[24];
    snprintf(buf, sizeof(buf), "%d dBm", (int)ap->rssi);
    lv_obj_t *rssi = lv_label_create(row);
    lv_label_set_text(rssi, buf);
    lv_obj_set_style_text_font(rssi, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(rssi, fw_theme_color_text_secondary(), 0);
    lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, -10, 0);

    return row;
}

static void fill_scan_list(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    if (s_scanning) {
        lv_obj_t *lb = lv_label_create(s_list);
        lv_label_set_text(lb, "正在扫描…");
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_secondary(), 0);
        return;
    }
    if (s_ap_count == 0) {
        lv_obj_t *lb = lv_label_create(s_list);
        lv_label_set_text(lb, "点右上角扫描网络");
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_secondary(), 0);
        return;
    }

    /* 当前连接的网络用强调色描边 + 强调色文字标出来 */
    svc_net_status_t st;
    memset(&st, 0, sizeof(st));
    const bool conn = (svc_net_get_status(&st) == ESP_OK) && st.wifi_connected;

    for (size_t i = 0; i < s_ap_count; i++) {
        lv_obj_t *row = make_ap_row(s_list, i);
        if (conn && strcmp(s_aps[i].ssid, st.wifi_ssid) == 0) {
            lv_obj_set_style_border_color(row, fw_theme_color_accent(), 0);
            lv_obj_t *name = lv_obj_get_child(row, 0);
            if (name != NULL) lv_obj_set_style_text_color(name, fw_theme_color_accent(), 0);
        }
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
        ESP_LOGW(TAG, "scan failed");
        s_ap_count = 0;
    }
    s_scanning = false;

    if (lvgl_port_lock(0)) {
        lv_async_call(scan_done_async, NULL);
        lvgl_port_unlock();
    }

    vTaskDelete(NULL);
}

static void scan_start(void)
{
    if (s_scanning) return;

    s_scanning = true;
    fill_scan_list();                       /* 先显示"正在扫描…" */

    if (xTaskCreate(scan_task, "wifi_scan", WIFI_SCAN_STACK, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        fill_scan_list();
        fw_ui_toast("扫描失败", 2000);
    }
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    scan_start();
}

/* -------------------------------- 设置页操作 -------------------------------- */

static void set_cb(lv_event_t *e)
{
    (void)e;
    page_show(PAGE_SET);
}

/* 断开 / 重连：断开时按已保存的凭据重连，不需要再输密码 */
static void disc_cb(lv_event_t *e)
{
    (void)e;

    svc_net_status_t st;
    memset(&st, 0, sizeof(st));
    const bool conn = (svc_net_get_status(&st) == ESP_OK) && st.wifi_connected;

    if (conn) {
        svc_net_wifi_disconnect();
    } else {
        char saved[33] = { 0 };
        if (svc_net_wifi_get_saved_ssid(saved, sizeof(saved)) == ESP_OK) {
            svc_net_wifi_auto_connect();
            fw_ui_toast("重连中…", 2000);
        } else {
            fw_ui_toast("没有已保存的网络", 2000);
        }
    }
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
        char msg[72];
        snprintf(msg, sizeof(msg), "热点 %s，浏览器访问 192.168.4.1",
                 SVC_NET_PROV_AP_SSID);
        fw_ui_toast(msg, 5000);
    }
    refresh_status();
}

/* SmartConfig：手机用 ESP 配网 App 把 Wi-Fi 信息广播给设备（区别于 AP + 网页配网） */
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

/* 忘记网络会清掉凭据，二次确认 */
static void forget_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    svc_net_wifi_forget();
    refresh_status();
    fw_ui_toast("已清除凭据", 2000);
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    fw_ui_dialog(NULL, "忘记网络", "将清除已保存的 Wi-Fi 凭据，确定吗？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, forget_confirm_cb, NULL);
}

static void on_wifi_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    refresh_status();
    lvgl_port_unlock();
}

/* -------------------------------- 页面搭建 -------------------------------- */

static lv_obj_t *make_page(lv_obj_t *host)
{
    lv_obj_t *page = lv_obj_create(host);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 6, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(page, false);
    return page;
}

static lv_obj_t *make_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 36, HEADER_H - 2);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 4);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, symbol);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
    return btn;
}

static void build_main_page(lv_obj_t *page)
{
    /* 头部：连接状态（撑满）+ 扫描 + 设置 */
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_size(bar, lv_pct(100), HEADER_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_state_lb = lv_label_create(bar);
    lv_obj_set_flex_grow(s_state_lb, 1);
    lv_label_set_text(s_state_lb, "");
    lv_obj_set_style_text_font(s_state_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state_lb, fw_theme_color_text_primary(), 0);

    make_icon_btn(bar, LV_SYMBOL_REFRESH, scan_cb);
    make_icon_btn(bar, LV_SYMBOL_SETTINGS, set_cb);

    /* 当前网络卡：SSID（大）+ 信号（右）；第二行 IP。未连接时整块隐藏 */
    s_card = lv_obj_create(page);
    lv_obj_set_size(s_card, lv_pct(100), CARD_H);
    lv_obj_set_style_bg_color(s_card, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(s_card, 8, 0);
    lv_obj_set_style_pad_all(s_card, 6, 0);
    lv_obj_set_scrollable(s_card, false);

    lv_obj_t *line1 = lv_obj_create(s_card);
    lv_obj_set_size(line1, lv_pct(100), 20);
    lv_obj_set_style_bg_opa(line1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line1, 0, 0);
    lv_obj_set_style_pad_all(line1, 0, 0);
    lv_obj_set_scrollable(line1, false);
    lv_obj_set_flex_flow(line1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(line1, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ssid_lb = lv_label_create(line1);
    lv_label_set_text(s_ssid_lb, "");
    lv_label_set_long_mode(s_ssid_lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ssid_lb, lv_pct(72));
    lv_obj_set_style_text_font(s_ssid_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_ssid_lb, fw_theme_color_text_primary(), 0);

    s_rssi_lb = lv_label_create(line1);
    lv_label_set_text(s_rssi_lb, "");
    lv_obj_set_style_text_font(s_rssi_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_rssi_lb, fw_theme_color_text_secondary(), 0);

    s_ip_lb = lv_label_create(s_card);
    lv_label_set_text(s_ip_lb, "");
    lv_obj_set_style_text_font(s_ip_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_ip_lb, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_ip_lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 可用网络：可滚动；未连接时状态卡隐藏，它会自动撑满剩余空间 */
    s_list = lv_obj_create(page);
    lv_obj_set_size(s_list, lv_pct(100), LIST_BOX_H);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
}

static void build_set_page(lv_obj_t *page)
{
    lv_obj_t *r;

    r = fw_ui_row_btn(page, LV_SYMBOL_REFRESH, "断开", disc_cb, NULL);
    lv_obj_set_height(r, SET_ROW_H);
    s_disc_row = r;

    r = fw_ui_row_btn(page, LV_SYMBOL_SETTINGS, "配网热点", prov_cb, NULL);
    lv_obj_set_height(r, SET_ROW_H);
    s_prov_row = r;

    r = fw_ui_row_btn(page, LV_SYMBOL_UPLOAD, "SmartConfig", smartconfig_cb, NULL);
    lv_obj_set_height(r, SET_ROW_H);
    s_sc_row = r;

    r = fw_ui_row_btn(page, LV_SYMBOL_TRASH, "忘记网络", forget_cb, NULL);
    lv_obj_set_height(r, SET_ROW_H);
    s_saved_row = r;
}

static void page_show(int page)
{
    s_page_cur = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_page[i] != NULL) lv_obj_set_hidden(s_page[i], i != page);
    }
    refresh_status();
}

/* ---------------------------------- 生命周期 ---------------------------------- */

static void *wifi_on_create(void)
{
    lvgl_port_lock(0);

    s_foreground = false;

    lv_obj_t *host = NULL;
    s_root = fw_ui_page(&host);

    for (int i = 0; i < PAGE_COUNT; i++) {
        s_page[i] = make_page(host);
    }
    build_main_page(s_page[PAGE_MAIN]);
    build_set_page(s_page[PAGE_SET]);

    page_show(PAGE_MAIN);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void wifi_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_CONNECTED, on_wifi_evt);
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_DISCONNECTED, on_wifi_evt);
}

/* on_start 与 on_resume 都挂：从桌面进来是 on_start，从返回栈回来是 on_resume */
static void wifi_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, on_wifi_evt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, on_wifi_evt, NULL);

    lvgl_port_lock(0);
    refresh_status();
    if (s_ap_count == 0 && !s_scanning) {
        scan_start();        /* 第一次进来列表是空的，自动扫一次 */
    }
    lvgl_port_unlock();
}

static void wifi_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_conn = NULL;                          /* 浮层挂在根屏上，随它一起删掉 */
    s_conn_ta = NULL;
    s_conn_kb = NULL;

    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_state_lb = NULL;
    s_card = NULL;
    s_ssid_lb = NULL;
    s_ip_lb = NULL;
    s_rssi_lb = NULL;
    s_list = NULL;
    s_disc_row = NULL;
    s_prov_row = NULL;
    s_sc_row = NULL;
    s_saved_row = NULL;
    for (int i = 0; i < PAGE_COUNT; i++) s_page[i] = NULL;
    lvgl_port_unlock();
}

/* 返回键：浮层开着先关浮层；在设置页则回主页；主页交给框架退出 App */
static bool wifi_on_back(void *ctx)
{
    (void)ctx;

    if (s_conn != NULL) {
        lvgl_port_lock(0);
        conn_close();
        lvgl_port_unlock();
        return true;
    }
    if (s_page_cur != PAGE_MAIN) {
        lvgl_port_lock(0);
        page_show(PAGE_MAIN);
        lvgl_port_unlock();
        return true;
    }
    return false;
}

const fw_app_desc_t app_wifi_desc = {
    .name = "Wi-Fi",
    .title = "Wi-Fi",
    .icon_64 = &icon_home_wifi,
    .symbol = LV_SYMBOL_WIFI,
    .on_create = wifi_on_create,
    .on_start = wifi_on_resume,
    .on_pause = wifi_on_pause,
    .on_resume = wifi_on_resume,
    .on_destroy = wifi_on_destroy,
    .on_back = wifi_on_back,
};
