/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Pairing 实现
 *
 * 两种提示：
 *   数字比对：对话框显示「配对码 123456，和对端显示的一致吗？」，确定/取消回给协议栈
 *   输入配对码：浮层 + 数字键盘（LVGL 键盘的 NUMBER 模式），输入对端显示的 6 位码
 * 协议栈自己的配对超时约 30 s，这里也挂一个同样的兜底定时器把提示收掉。
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "fw.pairing";

#define PAIR_TIMEOUT_MS  30000
#define KEYBOARD_H       120
#define OVERLAY_PAD      8

static lv_obj_t *s_prompt = NULL;      /* 输码浮层（数字比对用的是对话框，不留句柄） */
static lv_obj_t *s_ta = NULL;
static lv_obj_t *s_kb = NULL;          /* 键盘挂 lv_layer_top()，不进浮层的 flex 布局 */
static lv_timer_t *s_timer = NULL;
static bool s_dialog = false;          /* 当前提示是对话框（数字比对）还是输码浮层 */

/* ------------------------------- 收尾 ------------------------------- */

static void prompt_close(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_dialog) {
        fw_ui_dialog_close(NULL);
        s_dialog = false;
    }
    if (s_kb != NULL) {
        lv_obj_delete(s_kb);
        s_kb = NULL;
    }
    if (s_prompt != NULL) {
        lv_obj_delete(s_prompt);
        s_prompt = NULL;
        s_ta = NULL;
    }
}

/* 协议栈那边已经超时 / 用户没应答：收起提示（不回复，让协议栈自己判失败） */
static void timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_timer = NULL;                 /* 一次性定时器，LVGL 自己删 */
    ESP_LOGW(TAG, "pair prompt timed out");
    prompt_close();
}

/* ---------------------------- 数字比对 ---------------------------- */

static void compare_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    s_dialog = false;               /* 对话框已由 fw_ui 关掉 */

    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    svc_bt_pair_reply(btn == FW_DIALOG_BTN_OK, 0);
}

static void show_compare(uint32_t passkey)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "配对码 %06u，和对端显示的一致吗？", (unsigned)passkey);

    s_dialog = true;
    if (fw_ui_dialog(NULL, "蓝牙配对", msg,
                     FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, compare_cb, NULL) == NULL) {
        s_dialog = false;
        svc_bt_pair_reply(false, 0);    /* 弹不出来就直接拒绝，别让协议栈干等 */
    }
}

/* --------------------------- 输入配对码 --------------------------- */

static void enter_submit(void)
{
    char buf[8] = { 0 };
    if (s_ta != NULL) snprintf(buf, sizeof(buf), "%s", lv_textarea_get_text(s_ta));

    /* 必须是 6 位数字，否则不往下走 */
    uint32_t code = 0;
    bool ok = (strlen(buf) == 6);
    for (size_t i = 0; ok && i < 6; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
            ok = false;
        } else {
            code = code * 10 + (uint32_t)(buf[i] - '0');
        }
    }
    if (!ok) {
        fw_ui_toast("请输入 6 位配对码", 1500);
        return;
    }

    prompt_close();
    svc_bt_pair_reply(true, code);
}

static void enter_cancel_cb(lv_event_t *e)
{
    (void)e;
    prompt_close();
    svc_bt_pair_reply(false, 0);
}

static void kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        enter_submit();
    } else if (code == LV_EVENT_CANCEL) {
        enter_cancel_cb(NULL);
    }
}

static void show_enter(void)
{
    if (s_prompt != NULL) return;

    s_prompt = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_prompt);
    lv_obj_set_size(s_prompt, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_prompt, 0, 0);
    lv_obj_set_style_bg_color(s_prompt, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_prompt, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_prompt, OVERLAY_PAD, 0);
    lv_obj_set_style_pad_top(s_prompt, FW_STATUSBAR_H + OVERLAY_PAD, 0);
    lv_obj_set_style_pad_row(s_prompt, 8, 0);
    lv_obj_set_scrollable(s_prompt, false);
    lv_obj_set_flex_flow(s_prompt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_prompt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 第一行：提示（撑满）+ 取消 */
    lv_obj_t *row = lv_obj_create(s_prompt);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hint = lv_label_create(row);
    lv_obj_set_flex_grow(hint, 1);
    lv_label_set_text(hint, "输入对端显示的 6 位配对码");
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);

    fw_ui_btn(row, "取消", 52, false, enter_cancel_cb, NULL);

    /* 第二行：输入框（只收数字、最多 6 位） */
    s_ta = fw_ui_textarea(s_prompt, "6 位数字");
    lv_obj_set_width(s_ta, lv_pct(100));
    lv_textarea_set_accepted_chars(s_ta, "0123456789");
    lv_textarea_set_max_length(s_ta, 6);

    /* 键盘：挂 lv_layer_top()，不进浮层的 flex 布局；NUMBER 模式带「确定」键 */
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_set_size(s_kb, lv_display_get_horizontal_resolution(lv_display_get_default())
                          - 2 * OVERLAY_PAD, KEYBOARD_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -OVERLAY_PAD);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);

    s_timer = lv_timer_create(timeout_cb, PAIR_TIMEOUT_MS, NULL);
    if (s_timer != NULL) lv_timer_set_repeat_count(s_timer, 1);
}

/* ------------------------------- 事件 ------------------------------- */

/* 提示界面不在事件总线任务里搭：那里栈很小，建浮层 / 键盘这种深调用链会把它的栈压爆。
 * 只把参数拷出来，交给 LVGL 任务 */
static svc_bt_pair_mode_t s_pending_mode = SVC_BT_PAIR_COMPARE;
static uint32_t s_pending_passkey = 0;

static void prompt_ui_cb(void *arg)
{
    (void)arg;

    lvgl_port_lock(0);
    prompt_close();                 /* 同一时刻只留一个提示 */
    if (s_pending_mode == SVC_BT_PAIR_ENTER) {
        show_enter();
    } else {
        show_compare(s_pending_passkey);
    }
    lvgl_port_unlock();
}

static void pair_evt(const svc_event_t *evt, void *user)
{
    (void)user;

    if (evt->data == NULL || evt->data_len < sizeof(svc_bt_pair_prompt_t)) return;
    const svc_bt_pair_prompt_t *p = evt->data;

    /* 事件数据只在回调期间有效：先拷出来，再交给 LVGL 任务搭界面 */
    s_pending_mode = p->mode;
    s_pending_passkey = p->passkey;

    if (lvgl_port_lock(100)) {
        lv_async_call(prompt_ui_cb, NULL);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "lvgl busy, pair prompt dropped");
    }
}

esp_err_t fw_pairing_init(void)
{
    const esp_err_t err = svc_event_bus_subscribe(SVC_EVENT_BT_PAIR_PROMPT, pair_evt, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "subscribe failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}
