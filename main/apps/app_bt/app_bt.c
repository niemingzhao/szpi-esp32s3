/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Bluetooth（APP-BT 蓝牙）
 *
 * BLE 从机：显示协议栈状态、开关广播、扫描周边设备，并作为键鼠从机发送 HID 报告。
 * 状态与扫描结果经 svc_event_bus 通知（SVC_EVENT_BT_STATE_CHANGED / SVC_EVENT_BT_SCAN_DONE）。
 *
 * HID（NET-009）：svc_bt_init() 已初始化 HID 设备，主机配对后本页的「键盘 / 鼠标模拟」
 * 区可发送按键与鼠标动作。注意 HID 报告是"已配对加密"之后才允许发送的（见 svc_bt_hid.h），
 * 所以点击时用 svc_bt_hid_is_ready() 判定，未就绪只给 Toast 提示、不硬发。
 */

#include "app_bt.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "app.bt";

#define BT_SCAN_SECONDS     5
#define BT_SCAN_MAX         16

/* svc_bt_hid.h 只暴露了常用的一部分用法值，其余按 HID 规范补 */
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_SPACE       0x2C

/* 鼠标报告：左键位 bit0；固定小位移（相对坐标，-127 ~ 127） */
#define HID_MOUSE_BTN_LEFT  0x01
#define HID_MOUSE_MOVE_DX   20

/* 鼠标"单击"先发按下、30 ms 后补发松开，避免主机把按下状态一直保持 */
#define HID_MOUSE_RELEASE_MS 30

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_state_row = NULL;
static lv_obj_t *s_adv_row = NULL;
static lv_obj_t *s_list = NULL;

static void refresh_state(void)
{
    if (s_state_row == NULL) return;

    const svc_bt_state_t st = svc_bt_get_state();
    fw_ui_row_btn_value(s_state_row, svc_bt_state_name(st));
    fw_ui_row_btn_value(s_adv_row,
                        (st == SVC_BT_STATE_ADVERTISING) ? "广播中" : "已停止");
}

static void fill_scan_list(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    svc_bt_scan_result_t res[BT_SCAN_MAX];
    size_t count = 0;
    if (svc_bt_get_scan_results(res, BT_SCAN_MAX, &count) != ESP_OK) return;

    if (count == 0) {
        fw_ui_list_add(s_list, "未发现设备", NULL, NULL);
        return;
    }

    char line[80];
    for (size_t i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "%s  %d dBm",
                 res[i].name[0] ? res[i].name : "(未命名)", (int)res[i].rssi);
        fw_ui_list_add(s_list, line, NULL, NULL);
    }
}

static void adv_cb(lv_event_t *e)
{
    (void)e;

    const svc_bt_state_t st = svc_bt_get_state();
    if (st == SVC_BT_STATE_ADVERTISING) {
        svc_bt_adv_stop();
    } else if (st == SVC_BT_STATE_READY) {
        svc_bt_adv_start();
    } else {
        fw_ui_toast("已被连接", 1500);
    }
    refresh_state();
}

static void scan_cb(lv_event_t *e)
{
    (void)e;

    if (svc_bt_scan_start(BT_SCAN_SECONDS) != ESP_OK) {
        fw_ui_toast("扫描失败", 1500);
        return;
    }
    fw_ui_toast("正在扫描…", 1500);
}

/* ------------------------------ 键盘 / 鼠标模拟 ------------------------------ */

static void section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_secondary(), 0);
}

/* 发报告前的统一判定：未连接 / 未配对只提示，不硬发（服务层同样会拒绝） */
static bool hid_guard(void)
{
    if (!svc_bt_is_connected()) {
        fw_ui_toast("未连接，请先配对", 2000);
        return false;
    }
    if (!svc_bt_hid_is_ready()) {
        fw_ui_toast("配对未完成，请稍后重试", 2000);
        return false;
    }
    return true;
}

static void hid_key_cb(lv_event_t *e)
{
    const uint8_t usage = (uint8_t)(intptr_t)lv_event_get_user_data(e);

    if (!hid_guard()) return;
    if (svc_bt_hid_key_click(usage, 0) == ESP_OK) {
        fw_ui_toast("已发送", 1200);
    } else {
        fw_ui_toast("发送失败", 1500);
    }
}

static void hid_mouse_release_cb(lv_timer_t *t)
{
    (void)t;
    if (svc_bt_hid_is_ready()) {
        svc_bt_hid_mouse(0, 0, 0);
    }
}

static void hid_mouse_click_cb(lv_event_t *e)
{
    (void)e;

    if (!hid_guard()) return;
    if (svc_bt_hid_mouse(HID_MOUSE_BTN_LEFT, 0, 0) != ESP_OK) {
        fw_ui_toast("发送失败", 1500);
        return;
    }

    lv_timer_t *t = lv_timer_create(hid_mouse_release_cb, HID_MOUSE_RELEASE_MS, NULL);
    if (t != NULL) lv_timer_set_repeat_count(t, 1);
    fw_ui_toast("已发送", 1200);
}

static void hid_mouse_move_cb(lv_event_t *e)
{
    (void)e;

    if (!hid_guard()) return;
    if (svc_bt_hid_mouse(0, HID_MOUSE_MOVE_DX, 0) == ESP_OK) {
        fw_ui_toast("已发送", 1200);
    } else {
        fw_ui_toast("发送失败", 1500);
    }
}

static void fill_hid_rows(lv_obj_t *body)
{
    section_label(body, "键盘 / 鼠标模拟");

    fw_ui_row_btn(body, LV_SYMBOL_UP, "方向上", hid_key_cb,
                  (void *)(intptr_t)SVC_BT_HID_KEY_UP_ARROW);
    fw_ui_row_btn(body, LV_SYMBOL_DOWN, "方向下", hid_key_cb,
                  (void *)(intptr_t)SVC_BT_HID_KEY_DOWN_ARROW);
    fw_ui_row_btn(body, LV_SYMBOL_LEFT, "方向左", hid_key_cb,
                  (void *)(intptr_t)SVC_BT_HID_KEY_LEFT_ARROW);
    fw_ui_row_btn(body, LV_SYMBOL_RIGHT, "方向右", hid_key_cb,
                  (void *)(intptr_t)SVC_BT_HID_KEY_RIGHT_ARROW);
    fw_ui_row_btn(body, LV_SYMBOL_NEW_LINE, "回车", hid_key_cb,
                  (void *)(intptr_t)SVC_BT_HID_KEY_ENTER);
    fw_ui_row_btn(body, LV_SYMBOL_BACKSPACE, "退格", hid_key_cb,
                  (void *)(intptr_t)HID_KEY_BACKSPACE);
    fw_ui_row_btn(body, LV_SYMBOL_KEYBOARD, "空格", hid_key_cb,
                  (void *)(intptr_t)HID_KEY_SPACE);
    fw_ui_row_btn(body, LV_SYMBOL_OK, "鼠标左键", hid_mouse_click_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_RIGHT, "鼠标右移", hid_mouse_move_cb, NULL);
}

static void on_state_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    refresh_state();
    lvgl_port_unlock();
}

static void on_scan_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    fill_scan_list();
    lvgl_port_unlock();
}

static void *bt_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_state_row = fw_ui_row_btn(body, LV_SYMBOL_BLUETOOTH, "状态", NULL, NULL);
    s_adv_row = fw_ui_row_btn(body, LV_SYMBOL_PLAY, "广播", adv_cb, NULL);

    fw_ui_row_btn(body, LV_SYMBOL_REFRESH, "扫描设备", scan_cb, NULL);

    fill_hid_rows(body);

    s_list = fw_ui_list(body, "周边设备");

    refresh_state();
    fill_scan_list();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void bt_on_start(void *ctx)
{
    (void)ctx;
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, on_state_evt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BT_SCAN_DONE, on_scan_evt, NULL);

    lvgl_port_lock(0);
    refresh_state();
    lvgl_port_unlock();
}

static void bt_on_pause(void *ctx)
{
    (void)ctx;
    svc_event_bus_unsubscribe(SVC_EVENT_BT_STATE_CHANGED, on_state_evt);
    svc_event_bus_unsubscribe(SVC_EVENT_BT_SCAN_DONE, on_scan_evt);
}

static void bt_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_state_row = NULL;
    s_adv_row = NULL;
    s_list = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_bt_desc = {
    .name = "BT",
    .title = "蓝牙 BLE",
    .icon_64 = &icon_home_bt,
    .symbol = LV_SYMBOL_BLUETOOTH,
    .on_create = bt_on_create,
    .on_start = bt_on_start,
    .on_pause = bt_on_pause,
    .on_destroy = bt_on_destroy,
};
