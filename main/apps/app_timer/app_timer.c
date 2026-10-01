/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Timer（APP-TIMER 计时器）
 *
 * 倒计时：预设时长循环切换，到点弹提示并响一声（svc_audio_play_tone_async）。
 * 计时基于 esp_timer_get_time()（微秒，单调），不用系统时间。
 */

#include "app_timer.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "app.timer";

#define REFRESH_MS      200
#define TONE_HZ         880
#define TONE_MS         400

/* 预设时长（秒） */
static const uint32_t PRESETS[] = { 60, 180, 300, 600, 900, 1800 };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time_lb = NULL;
static lv_obj_t *s_dur_row = NULL;
static lv_obj_t *s_run_lb = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_running = false;
static bool s_done = false;
static int64_t s_start_us = 0;      /* 本次开始时刻 */
static int64_t s_elapsed_us = 0;    /* 已经计掉的时长 */
static uint32_t s_total_s = 60;

static int64_t remain_us(void)
{
    int64_t used = s_elapsed_us;
    if (s_running) used += esp_timer_get_time() - s_start_us;

    const int64_t total = (int64_t)s_total_s * 1000000;
    return (used >= total) ? 0 : (total - used);
}

static void show_duration(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%u 分 %u 秒",
             (unsigned)(s_total_s / 60), (unsigned)(s_total_s % 60));
    if (s_dur_row != NULL) fw_ui_row_btn_value(s_dur_row, buf);
}

static void show_remain(void)
{
    if (s_time_lb == NULL) return;

    const int64_t us = remain_us();
    const int sec = (int)((us + 999999) / 1000000);   /* 向上取整，最后 1 秒不早跳 0 */
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
    lv_label_set_text(s_time_lb, buf);

    if (s_run_lb != NULL) {
        lv_label_set_text(s_run_lb, s_running ? "暂停" : "开始");
    }
}

static void done_notify(void)
{
    s_done = true;
    s_running = false;
    s_elapsed_us = (int64_t)s_total_s * 1000000;

    fw_ui_toast("时间到", 3000);
    svc_audio_play_tone_async(TONE_HZ, TONE_MS);
    show_remain();
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    if (s_running && remain_us() == 0) {
        done_notify();
        return;
    }
    if (!s_done) {
        show_remain();
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 40);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
    return btn;
}

static void duration_cb(lv_event_t *e)
{
    (void)e;

    const size_t n = sizeof(PRESETS) / sizeof(PRESETS[0]);
    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        if (PRESETS[i] == s_total_s) {
            idx = i;
            break;
        }
    }
    s_total_s = PRESETS[(idx + 1) % n];

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;

    show_duration();
    show_remain();
}

static void run_cb(lv_event_t *e)
{
    (void)e;

    if (s_done) {
        s_elapsed_us = 0;
        s_done = false;
    }

    if (s_running) {
        s_elapsed_us += esp_timer_get_time() - s_start_us;
        s_running = false;
    } else {
        s_start_us = esp_timer_get_time();
        s_running = true;
    }
    show_remain();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;
    show_remain();
}

static void *timer_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_time_lb = lv_label_create(body);
    lv_label_set_text(s_time_lb, "01:00");
    lv_obj_set_style_text_font(s_time_lb, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_time_lb, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_time_lb, LV_ALIGN_CENTER, 0, -28);

    s_dur_row = fw_ui_row_btn(body, LV_SYMBOL_LOOP, "时长", duration_cb, NULL);
    show_duration();

    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *run = make_btn(row, "开始", 120, run_cb);
    lv_obj_align(run, LV_ALIGN_LEFT_MID, 0, 0);
    s_run_lb = lv_obj_get_child(run, 0);

    lv_obj_t *rst = make_btn(row, "复位", 120, reset_cb);
    lv_obj_align(rst, LV_ALIGN_RIGHT_MID, 0, 0);

    show_remain();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void timer_on_start(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer == NULL) {
        s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    }
    lvgl_port_unlock();
}

static void timer_on_pause(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lvgl_port_unlock();
}

static void timer_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_time_lb = NULL;
    s_dur_row = NULL;
    s_run_lb = NULL;
    lvgl_port_unlock();

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;
}

const fw_app_desc_t app_timer_desc = {
    .name = "Timer",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_LOOP,
    .on_create = timer_on_create,
    .on_start = timer_on_start,
    .on_pause = timer_on_pause,
    .on_destroy = timer_on_destroy,
};
