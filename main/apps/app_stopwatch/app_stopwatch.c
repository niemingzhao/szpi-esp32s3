/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Stopwatch（APP-STOPWATCH 秒表）
 *
 * 计时基于 esp_timer_get_time()（微秒，单调），不用系统时间，改时间也不受影响。
 * 50 ms 刷新一次显示。计次列表显示每一圈的分段用时（本次用时 = 本次累计 − 上次累计）。
 * 计次数据放在堆上（LAP_MAX × 8 B），不在 App 里加大静态数组（见 AGENTS 4.20）。
 */

#include "app_stopwatch.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "app.stopwatch";

#define REFRESH_MS      50
#define LAP_MAX         30

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time_lb = NULL;
static lv_obj_t *s_state_lb = NULL;
static lv_obj_t *s_run_lb = NULL;
static lv_obj_t *s_lap_list = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_running = false;
static int64_t s_start_us = 0;      /* 本次开始时刻 */
static int64_t s_accum_us = 0;      /* 之前累计 */

/* 每次按"计次"时记录的累计用时（微秒），堆上分配 */
static int64_t *s_laps = NULL;
static size_t s_lap_count = 0;

static int64_t elapsed_us(void)
{
    return s_accum_us + (s_running ? (esp_timer_get_time() - s_start_us) : 0);
}

/* 把微秒格式化成 分:秒.百分秒 */
static void format_time(int64_t us, char *buf, size_t len)
{
    const int64_t cs = us / 10000;        /* 百分秒 */
    snprintf(buf, len, "%02d:%02d.%02d",
             (int)(cs / 6000), (int)((cs / 100) % 60), (int)(cs % 100));
}

static void show_elapsed(void)
{
    if (s_time_lb == NULL) return;

    char buf[16];
    format_time(elapsed_us(), buf, sizeof(buf));
    lv_label_set_text(s_time_lb, buf);
}

static void show_state(void)
{
    if (s_state_lb != NULL) {
        lv_label_set_text(s_state_lb, s_running ? "计时中" : "已暂停");
    }
    if (s_run_lb != NULL) {
        lv_label_set_text(s_run_lb, s_running ? "暂停" : "开始");
    }
}

/* 只删除列表条目，保留 fw_ui_list 的标题（标题是第 0 个子对象） */
static void list_clear_items(lv_obj_t *list)
{
    if (list == NULL) return;

    uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = n; i > 1; i--) {
        lv_obj_delete(lv_obj_get_child(list, (int32_t)(i - 1)));
    }
}

/* 重建计次列表（每次计次 / 复位时调用，条目不多，整表重排即可） */
static void fill_laps(void)
{
    if (s_lap_list == NULL) return;

    list_clear_items(s_lap_list);

    if (s_laps == NULL) {
        fw_ui_list_add(s_lap_list, "内存不足", NULL, NULL);
        return;
    }
    if (s_lap_count == 0) {
        fw_ui_list_add(s_lap_list, "无计次", NULL, NULL);
        return;
    }

    char t[16];
    char line[40];
    for (size_t i = 0; i < s_lap_count; i++) {
        const int64_t prev = (i == 0) ? 0 : s_laps[i - 1];
        format_time(s_laps[i] - prev, t, sizeof(t));
        snprintf(line, sizeof(line), "第 %u 次  %s", (unsigned)(i + 1), t);
        fw_ui_list_add(s_lap_list, line, NULL, NULL);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    show_elapsed();
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

static void lap_cb(lv_event_t *e)
{
    (void)e;

    if (!s_running) {
        fw_ui_toast("计时未开始", 2000);
        return;
    }
    if (s_laps == NULL) {
        fw_ui_toast("内存不足", 2000);
        return;
    }
    if (s_lap_count >= LAP_MAX) {
        fw_ui_toast("计次已满", 2000);
        return;
    }

    s_laps[s_lap_count++] = elapsed_us();
    fill_laps();
}

static void run_cb(lv_event_t *e)
{
    (void)e;

    if (s_running) {
        s_accum_us = elapsed_us();
        s_running = false;
    } else {
        s_start_us = esp_timer_get_time();
        s_running = true;
    }
    show_state();
    show_elapsed();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;

    s_running = false;
    s_accum_us = 0;
    s_lap_count = 0;            /* 复位清空计次（暂停时保留） */
    show_state();
    show_elapsed();
    fill_laps();
}

static void *stopwatch_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_laps == NULL) {
        s_laps = malloc(sizeof(int64_t) * LAP_MAX);
    }
    s_lap_count = 0;

    s_time_lb = lv_label_create(body);
    lv_label_set_text(s_time_lb, "00:00.00");
    lv_obj_set_style_text_font(s_time_lb, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_time_lb, fw_theme_color_text_primary(), 0);

    s_state_lb = lv_label_create(body);
    lv_obj_set_style_text_font(s_state_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state_lb, fw_theme_color_text_secondary(), 0);

    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *run = make_btn(row, "开始", 88, run_cb);
    s_run_lb = lv_obj_get_child(run, 0);

    make_btn(row, "计次", 88, lap_cb);
    make_btn(row, "复位", 88, reset_cb);

    s_lap_list = fw_ui_list(body, "计次");
    lv_obj_set_width(s_lap_list, lv_pct(100));
    lv_obj_set_flex_grow(s_lap_list, 1);

    show_state();
    show_elapsed();
    fill_laps();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void stopwatch_on_start(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer == NULL) {
        s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    }
    lvgl_port_unlock();
}

static void stopwatch_on_pause(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lvgl_port_unlock();
}

static void stopwatch_on_destroy(void *ctx)
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
    s_state_lb = NULL;
    s_run_lb = NULL;
    s_lap_list = NULL;
    lvgl_port_unlock();

    free(s_laps);
    s_laps = NULL;
    s_lap_count = 0;

    s_running = false;
    s_accum_us = 0;
}

const fw_app_desc_t app_stopwatch_desc = {
    .name = "Stopwatch",
    .title = "秒表",
    .icon_64 = &icon_home_stopwatch,
    .symbol = LV_SYMBOL_PLAY,
    .on_create = stopwatch_on_create,
    .on_start = stopwatch_on_start,
    .on_pause = stopwatch_on_pause,
    .on_destroy = stopwatch_on_destroy,
};
