/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Settings
 *
 * 单屏多页：主菜单 / Wi-Fi / 输入密码 / 显示 / 关于。
 * 不设应用内返回按钮：状态栏返回键（或 BOOT 单击）由 on_back 处理，
 * 有上一级页面就回上一级，已经在主菜单则交给 fw_app_mgr 退出到桌面。
 * Wi-Fi 扫描是阻塞操作，放在独立任务里跑，扫描完再回填列表，避免卡住 UI。
 */

#include "app_settings.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.settings";

#define PAGE_W      320
#define PAGE_H      (240 - FW_STATUSBAR_H)
#define AP_MAX      16
#define AP_SSID_LEN 33

typedef enum {
    PAGE_MENU = 0,
    PAGE_WIFI,
    PAGE_WIFI_PASS,
    PAGE_DISPLAY,
    PAGE_ABOUT,
    PAGE_COUNT
} settings_page_t;

/* 每页的"上一级" */
static const settings_page_t k_back[PAGE_COUNT] = {
    PAGE_MENU, PAGE_MENU, PAGE_WIFI, PAGE_MENU, PAGE_MENU
};

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page[PAGE_COUNT];
static settings_page_t s_cur = PAGE_MENU;

/* 主菜单 */
static lv_obj_t *s_menu_wifi_label = NULL;
/* Wi-Fi 页 */
static lv_obj_t *s_wifi_state = NULL;
static lv_obj_t *s_ap_list = NULL;
static lv_obj_t *s_scan_label = NULL;
static volatile bool s_scanning = false;
static char s_ap_ssid[AP_MAX][AP_SSID_LEN];
/* 密码页 */
static lv_obj_t *s_pass_ta = NULL;
static lv_obj_t *s_pass_hint = NULL;
static char s_pending_ssid[AP_SSID_LEN];
/* 显示页 */
static lv_obj_t *s_bright_slider = NULL;
static lv_obj_t *s_bright_value = NULL;
static lv_obj_t *s_timeout_value = NULL;
static bool s_bright_sync = false;

/* ---------------------------------- 通用 ---------------------------------- */

/* 页面不设标题栏：状态栏已有返回键，把竖直空间全留给内容 */
static lv_obj_t *page_new(void)
{
    lv_obj_t *p = lv_obj_create(s_root);
    lv_obj_set_size(p, PAGE_W, PAGE_H);
    lv_obj_set_pos(p, 0, FW_STATUSBAR_H);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);

    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* 内容区：整页纵向可滚动 */
static lv_obj_t *content_of(lv_obj_t *page)
{
    lv_obj_t *c = lv_obj_create(page);
    lv_obj_set_size(c, PAGE_W, PAGE_H);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_AUTO);
    return c;
}

static void show_page(settings_page_t page)
{
    if (page >= PAGE_COUNT) return;
    s_cur = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_page[i] == NULL) continue;
        if ((settings_page_t)i == page) lv_obj_clear_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        else                            lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *card_row(lv_obj_t *parent, const char *symbol, const char *text,
                          lv_event_cb_t cb, void *user, lv_coord_t h)
{
    lv_obj_t *row = lv_btn_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, h);
    lv_obj_set_style_bg_color(row, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    if (cb != NULL) lv_obj_add_event_cb(row, cb, LV_EVENT_SHORT_CLICKED, user);

    if (symbol != NULL) {
        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, fw_asset_font_20(), 0);
        lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
    }

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, (symbol != NULL) ? 34 : 10, 0);
    return label;
}

static lv_obj_t *small_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user,
                           lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_secondary(), 0);
    return l;
}

/* ------------------------------- Wi-Fi 状态 ------------------------------- */

static void update_wifi_ui(void)
{
    svc_net_status_t st;
    bool conn = (svc_net_get_status(&st) == ESP_OK) && st.wifi_connected;

    char saved[AP_SSID_LEN] = { 0 };
    bool has_saved = (svc_net_wifi_get_saved_ssid(saved, sizeof(saved)) == ESP_OK);

    if (s_wifi_state != NULL) {
        if (conn) {
            lv_label_set_text_fmt(s_wifi_state, "已连接 %s\nIP %s   %d dBm",
                                  st.wifi_ssid, st.ip_addr, (int)st.rssi);
        } else if (has_saved) {
            lv_label_set_text_fmt(s_wifi_state, "未连接 · 已保存 %s", saved);
        } else {
            lv_label_set_text(s_wifi_state, "未连接 · 未保存网络");
        }
    }

    if (s_menu_wifi_label != NULL) {
        lv_label_set_text_fmt(s_menu_wifi_label, "Wi-Fi   %s",
                              conn ? st.wifi_ssid : (has_saved ? saved : "未连接"));
    }
}

/* --------------------------------- Wi-Fi 页 -------------------------------- */

static void ap_click_cb(lv_event_t *e)
{
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= AP_MAX) return;

    strlcpy(s_pending_ssid, s_ap_ssid[idx], sizeof(s_pending_ssid));

    /* 用 SSID 当输入框提示：不占额外竖直空间，也能看清在连哪个网络 */
    if (s_pass_ta != NULL) {
        lv_textarea_set_placeholder_text(s_pass_ta, s_pending_ssid);
        lv_textarea_set_text(s_pass_ta, "");
    }
    if (s_pass_hint != NULL) lv_label_set_text(s_pass_hint, "");

    show_page(PAGE_WIFI_PASS);
}

static void ap_list_fill(svc_net_wifi_ap_t *aps, size_t found, esp_err_t err)
{
    if (s_ap_list == NULL) return;

    lv_obj_clean(s_ap_list);

    if (err == ESP_ERR_TIMEOUT) {
        fw_ui_list_add(s_ap_list, "扫描超时", NULL, NULL);
        return;
    }
    if (err != ESP_OK) {
        fw_ui_list_add(s_ap_list, "扫描失败", NULL, NULL);
        return;
    }
    if (found == 0) {
        fw_ui_list_add(s_ap_list, "未发现热点", NULL, NULL);
        return;
    }

    for (size_t i = 0; i < found && i < AP_MAX; i++) {
        strlcpy(s_ap_ssid[i], aps[i].ssid, sizeof(s_ap_ssid[i]));
        char text[64];
        snprintf(text, sizeof(text), "%s   %d dBm", aps[i].ssid, (int)aps[i].rssi);
        fw_ui_list_add(s_ap_list, text, ap_click_cb, (void *)(intptr_t)i);
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    svc_net_wifi_ap_t aps[AP_MAX];
    size_t found = 0;
    esp_err_t err = svc_net_wifi_scan(aps, AP_MAX, &found, 8000);

    lvgl_port_lock(0);
    ap_list_fill(aps, found, err);
    if (s_scan_label != NULL) lv_label_set_text(s_scan_label, "扫描");
    s_scanning = false;
    lvgl_port_unlock();

    vTaskDelete(NULL);
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    if (s_scanning) return;

    svc_net_wifi_start(SVC_NET_MODE_STA);   /* 扫描需要 Wi-Fi 已启动 */

    s_scanning = true;
    if (s_scan_label != NULL) lv_label_set_text(s_scan_label, "扫描中");
    if (s_ap_list != NULL) {
        lv_obj_clean(s_ap_list);
        fw_ui_list_add(s_ap_list, "扫描中…", NULL, NULL);
    }

    if (xTaskCreatePinnedToCore(scan_task, "set_scan", 5120, NULL, 4, NULL, 0) != pdPASS) {
        s_scanning = false;
        if (s_scan_label != NULL) lv_label_set_text(s_scan_label, "扫描");
        ESP_LOGW(TAG, "scan task create failed");
    }
}

/* 用已保存的凭据重连，不用再输密码 */
static void reconnect_cb(lv_event_t *e)
{
    (void)e;

    /* auto_connect 内部会按需启动 Wi-Fi，这里不重复 start */
    esp_err_t err = svc_net_wifi_auto_connect();
    if (err == ESP_ERR_NOT_FOUND) {
        fw_ui_toast("没有已保存的网络", 2000);
    } else if (err != ESP_OK) {
        fw_ui_toast(esp_err_to_name(err), 2000);
    } else {
        fw_ui_toast("正在连接已保存的网络…", 2000);
    }
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    svc_net_wifi_forget();
    if (s_ap_list != NULL) lv_obj_clean(s_ap_list);
    update_wifi_ui();
    fw_ui_toast("已忘记网络", 2000);
}

static void build_wifi(lv_obj_t *page)
{
    lv_obj_t *c = content_of(page);

    s_wifi_state = lv_label_create(c);
    lv_label_set_text(s_wifi_state, "未连接");
    lv_obj_set_style_text_font(s_wifi_state, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_wifi_state, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_text_line_space(s_wifi_state, 4, 0);

    lv_obj_t *row = lv_obj_create(c);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 34);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

    lv_obj_t *scan = small_btn(row, "扫描", scan_cb, NULL, 92, 34);
    s_scan_label = lv_obj_get_child(scan, 0);
    small_btn(row, "连接", reconnect_cb, NULL, 92, 34);
    small_btn(row, "忘记网络", forget_cb, NULL, 92, 34);

    s_ap_list = fw_ui_list(c, NULL);
    lv_obj_set_width(s_ap_list, lv_pct(100));
    lv_obj_set_flex_grow(s_ap_list, 1);
    lv_obj_set_style_pad_all(s_ap_list, 6, 0);

    update_wifi_ui();
}

/* -------------------------------- 密码输入页 -------------------------------- */

static void connect_cb(lv_event_t *e)
{
    (void)e;

    svc_net_wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    strlcpy(creds.ssid, s_pending_ssid, sizeof(creds.ssid));
    if (s_pass_ta != NULL) {
        strlcpy(creds.password, lv_textarea_get_text(s_pass_ta), sizeof(creds.password));
    }

    svc_net_wifi_start(SVC_NET_MODE_STA);
    esp_err_t err = svc_net_wifi_connect(&creds);
    if (s_pass_hint != NULL) {
        lv_label_set_text(s_pass_hint, (err == ESP_OK) ? "连接中…" : esp_err_to_name(err));
    }
}

static void show_pass_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    if (s_pass_ta != NULL) {
        lv_textarea_set_password_mode(s_pass_ta, !lv_obj_has_state(cb, LV_STATE_CHECKED));
    }
}

static void build_pass(lv_obj_t *page)
{
    s_pass_ta = lv_textarea_create(page);
    lv_obj_set_size(s_pass_ta, 196, 34);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 12, 8);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, "密码");
    lv_obj_set_style_text_font(s_pass_ta, fw_asset_font_cn(), 0);
    /* 显式设色：不依赖 LVGL 自带主题（它只在启动时定一次） */
    lv_obj_set_style_bg_color(s_pass_ta, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_text_color(s_pass_ta, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_border_width(s_pass_ta, 1, 0);
    lv_obj_set_style_border_color(s_pass_ta, fw_theme_color_border(), 0);
    /* 提示文字（进入密码页时会被换成 SSID）用次要文字色：主题在深色下的默认值偏暗 */
    lv_obj_set_style_text_color(s_pass_ta, fw_theme_color_text_secondary(), LV_PART_TEXTAREA_PLACEHOLDER);

    lv_obj_t *btn = small_btn(page, "连接", connect_cb, NULL, 92, 34);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 216, 8);

    lv_obj_t *show = lv_checkbox_create(page);
    lv_checkbox_set_text(show, "显示密码");
    lv_obj_set_style_text_font(show, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(show, fw_theme_color_text_secondary(), 0);
    lv_obj_align(show, LV_ALIGN_TOP_LEFT, 12, 50);
    lv_obj_add_event_cb(show, show_pass_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_pass_hint = lv_label_create(page);
    lv_label_set_text(s_pass_hint, "");
    lv_obj_set_style_text_font(s_pass_hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_pass_hint, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_pass_hint, LV_ALIGN_TOP_LEFT, 130, 54);

    /* 键盘贴在底部；上方内容都排在键盘之上，不会被遮挡 */
    lv_obj_t *kb = lv_keyboard_create(page);
    lv_obj_set_size(kb, PAGE_W, 120);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pass_ta);
}

/* --------------------------------- 显示页 --------------------------------- */

static void bright_cb(lv_event_t *e)
{
    if (s_bright_sync) return;
    lv_obj_t *s = lv_event_get_target(e);
    uint8_t v = (uint8_t)lv_slider_get_value(s);
    svc_power_set_brightness(v);
    if (s_bright_value != NULL) lv_label_set_text_fmt(s_bright_value, "%u%%", (unsigned)v);
}

static void evt_brightness(const svc_event_t *evt, void *user)
{
    (void)user;
    if (s_bright_slider == NULL || evt->data == NULL || evt->data_len < sizeof(uint8_t)) return;

    uint8_t v = *(const uint8_t *)evt->data;
    lvgl_port_lock(0);
    s_bright_sync = true;
    lv_slider_set_value(s_bright_slider, (int32_t)v, LV_ANIM_OFF);
    s_bright_sync = false;
    if (s_bright_value != NULL) lv_label_set_text_fmt(s_bright_value, "%u%%", (unsigned)v);
    lvgl_port_unlock();
}

static void timeout_cb(lv_event_t *e)
{
    uint32_t sec = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    svc_power_set_backlight_timeout(sec);
    if (s_timeout_value != NULL) {
        if (sec == 0) lv_label_set_text(s_timeout_value, "背光超时  常亮");
        else          lv_label_set_text_fmt(s_timeout_value, "背光超时  %u 秒", (unsigned)sec);
    }
}

static void theme_cb(lv_event_t *e)
{
    fw_theme_t t = (fw_theme_t)(intptr_t)lv_event_get_user_data(e);
    fw_theme_apply(t);
    fw_ui_toast("主题已切换", 1500);
}

static void build_display(lv_obj_t *page)
{
    lv_obj_t *c = content_of(page);

    /* 亮度 */
    lv_obj_t *head = lv_obj_create(c);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, 22);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *bl = caption(head, "亮度");
    s_bright_value = lv_label_create(head);
    lv_label_set_text_fmt(s_bright_value, "%u%%", (unsigned)svc_power_get_brightness());
    lv_obj_set_style_text_font(s_bright_value, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_bright_value, fw_theme_color_text_primary(), 0);

    s_bright_slider = lv_slider_create(c);
    lv_obj_set_width(s_bright_slider, lv_pct(100));
    lv_obj_set_height(s_bright_slider, 16);
    lv_slider_set_range(s_bright_slider, 0, 100);
    lv_slider_set_value(s_bright_slider, (int32_t)svc_power_get_brightness(), LV_ANIM_OFF);
    /* 显式设色：轨道用边框色、指示条与圆点用主色 */
    lv_obj_set_style_bg_color(s_bright_slider, fw_theme_color_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bright_slider, fw_theme_color_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bright_slider, fw_theme_color_accent(), LV_PART_KNOB);
    lv_obj_add_event_cb(s_bright_slider, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 背光超时 */
    uint32_t to = svc_power_get_backlight_timeout();
    char totext[48];
    if (to == 0) snprintf(totext, sizeof(totext), "背光超时  常亮");
    else         snprintf(totext, sizeof(totext), "背光超时  %u 秒", (unsigned)to);
    s_timeout_value = caption(c, totext);

    lv_obj_t *trow = lv_obj_create(c);
    lv_obj_set_width(trow, lv_pct(100));
    lv_obj_set_height(trow, 34);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(trow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(trow, 0, 0);
    lv_obj_set_style_pad_all(trow, 0, 0);
    lv_obj_set_style_pad_column(trow, 8, 0);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    small_btn(trow, "15 秒", timeout_cb, (void *)(intptr_t)15, 68, 34);
    small_btn(trow, "30 秒", timeout_cb, (void *)(intptr_t)30, 68, 34);
    small_btn(trow, "1 分钟", timeout_cb, (void *)(intptr_t)60, 68, 34);
    small_btn(trow, "常亮", timeout_cb, (void *)(intptr_t)0, 68, 34);

    /* 主题 */
    caption(c, "主题");

    lv_obj_t *throw = lv_obj_create(c);
    lv_obj_set_width(throw, lv_pct(100));
    lv_obj_set_height(throw, 34);
    lv_obj_clear_flag(throw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(throw, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(throw, 0, 0);
    lv_obj_set_style_pad_all(throw, 0, 0);
    lv_obj_set_style_pad_column(throw, 8, 0);
    lv_obj_set_flex_flow(throw, LV_FLEX_FLOW_ROW);
    small_btn(throw, "深色", theme_cb, (void *)(intptr_t)FW_THEME_DARK, 88, 34);
    small_btn(throw, "浅色", theme_cb, (void *)(intptr_t)FW_THEME_LIGHT, 88, 34);
}

/* --------------------------------- 关于页 --------------------------------- */

static void build_about(lv_obj_t *page)
{
    lv_obj_t *c = content_of(page);

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    char buf[320];
    snprintf(buf, sizeof(buf),
             "SZPI-OS %s\n"
             "ESP-IDF %s\n"
             "芯片 ESP32-S3，%d 核\n"
             "内部 RAM 剩余 %u KB\n"
             "PSRAM 剩余 %u KB",
             SZPI_OS_VERSION, esp_get_idf_version(), ci.cores,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, buf);
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_line_space(l, 6, 0);
}

/* --------------------------------- 主菜单 --------------------------------- */

static void menu_cb(lv_event_t *e)
{
    show_page((settings_page_t)(intptr_t)lv_event_get_user_data(e));
}

static void build_menu(lv_obj_t *page)
{
    lv_obj_t *c = content_of(page);

    s_menu_wifi_label = card_row(c, LV_SYMBOL_WIFI, "Wi-Fi",
                                 menu_cb, (void *)(intptr_t)PAGE_WIFI, 50);
    card_row(c, LV_SYMBOL_TINT, "显示", menu_cb, (void *)(intptr_t)PAGE_DISPLAY, 50);
    card_row(c, LV_SYMBOL_LIST, "关于", menu_cb, (void *)(intptr_t)PAGE_ABOUT, 50);
}

/* ------------------------------- 事件订阅 ------------------------------- */

static void evt_wifi(const svc_event_t *evt, void *user)
{
    (void)user;
    lvgl_port_lock(0);
    update_wifi_ui();
    if (evt->id == SVC_EVENT_WIFI_CONNECTED) {
        if (s_cur == PAGE_WIFI_PASS) show_page(PAGE_WIFI);
        fw_ui_toast("Wi-Fi 已连接", 2000);
    } else if (evt->id == SVC_EVENT_WIFI_CONNECT_FAILED) {
        if (s_pass_hint != NULL) lv_label_set_text(s_pass_hint, "连接失败");
        fw_ui_toast("Wi-Fi 连接失败", 2500);
    }
    lvgl_port_unlock();
}

/* --------------------------------- 生命周期 --------------------------------- */

static void *settings_on_create(void)
{
    lvgl_port_lock(0);

    s_root = lv_obj_create(NULL);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_root, fw_theme_color_bg_primary(), 0);

    for (int i = 0; i < PAGE_COUNT; i++) s_page[i] = NULL;
    s_page[PAGE_MENU] = page_new();
    s_page[PAGE_WIFI] = page_new();
    s_page[PAGE_WIFI_PASS] = page_new();
    s_page[PAGE_DISPLAY] = page_new();
    s_page[PAGE_ABOUT] = page_new();

    build_menu(s_page[PAGE_MENU]);
    build_wifi(s_page[PAGE_WIFI]);
    build_pass(s_page[PAGE_WIFI_PASS]);
    build_display(s_page[PAGE_DISPLAY]);
    build_about(s_page[PAGE_ABOUT]);

    show_page(PAGE_MENU);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void settings_on_start(void *ctx)
{
    (void)ctx;
    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECT_FAILED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BRIGHTNESS_CHANGED, evt_brightness, NULL);

    lvgl_port_lock(0);
    update_wifi_ui();
    show_page(PAGE_MENU);
    lvgl_port_unlock();
}

static void settings_on_pause(void *ctx)
{
    (void)ctx;
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_CONNECTED, evt_wifi);
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_CONNECT_FAILED, evt_wifi);
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_DISCONNECTED, evt_wifi);
    svc_event_bus_unsubscribe(SVC_EVENT_BRIGHTNESS_CHANGED, evt_brightness);
}

static void settings_on_resume(void *ctx)
{
    settings_on_start(ctx);
}

/* 返回：有上一级就回上一级，已在主菜单则交给 fw_app_mgr 退出 App */
static bool settings_on_back(void *ctx)
{
    (void)ctx;
    if (s_cur == PAGE_MENU) return false;
    show_page(k_back[s_cur]);
    return true;
}

static void settings_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
        for (int i = 0; i < PAGE_COUNT; i++) s_page[i] = NULL;
        s_menu_wifi_label = NULL;
        s_wifi_state = NULL;
        s_ap_list = NULL;
        s_scan_label = NULL;
        s_pass_ta = NULL;
        s_pass_hint = NULL;
        s_bright_slider = NULL;
        s_bright_value = NULL;
        s_timeout_value = NULL;
    }
    lvgl_port_unlock();
}

const fw_app_desc_t app_settings_desc = {
    .name = "Settings",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_SETTINGS,
    .on_create = settings_on_create,
    .on_start = settings_on_start,
    .on_pause = settings_on_pause,
    .on_resume = settings_on_resume,
    .on_destroy = settings_on_destroy,
    .on_back = settings_on_back,
};
