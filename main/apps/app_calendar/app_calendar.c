/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Calendar（APP-CALENDAR 日历）
 *
 * 月视图：7 列网格，可切上 / 下月，今天用强调色标出。
 * 时间全部来自 svc_time（未 NTP 同步时按 1970 起算，界面会显示 1970 年）。
 */

#include "app_calendar.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "app.calendar";

#define CELL_W      40
#define CELL_H      22
#define WEEK_DAYS   7

static const char *WEEK[] = { "日", "一", "二", "三", "四", "五", "六" };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_grid = NULL;

static int s_year = 1970;
static int s_month = 1;         /* 1-12 */
static int s_today = 0;         /* 今天几号，非本月为 0 */

static void month_bounds(int year, int month, int64_t *first, int *days)
{
    struct tm tm = { 0 };
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = 1;
    const time_t start = mktime(&tm);

    tm.tm_mon = month;          /* 下月 1 号，用来算本月天数 */
    tm.tm_mday = 1;
    const time_t next = mktime(&tm);

    *first = (int64_t)start;
    *days = (int)((next - start) / 86400);
}

static void build_month(void)
{
    if (s_grid == NULL) return;

    lv_obj_clean(s_grid);

    int64_t first = 0;
    int days = 0;
    month_bounds(s_year, s_month, &first, &days);

    struct tm tm = { 0 };
    time_t t = (time_t)first;
    localtime_r(&t, &tm);
    const int lead = tm.tm_wday;        /* 1 号是周几，0 = 周日 */

    char buf[24];
    snprintf(buf, sizeof(buf), "%d 年 %d 月", s_year, s_month);
    if (s_title != NULL) lv_label_set_text(s_title, buf);

    for (int i = 0; i < WEEK_DAYS; i++) {
        lv_obj_t *lb = lv_label_create(s_grid);
        lv_obj_set_size(lb, CELL_W, CELL_H);
        lv_label_set_text(lb, WEEK[i]);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_secondary(), 0);
    }

    for (int i = 0; i < WEEK_DAYS * 6; i++) {
        const int day = i - lead + 1;
        const bool valid = (day >= 1 && day <= days);

        lv_obj_t *lb = lv_label_create(s_grid);
        lv_obj_set_size(lb, CELL_W, CELL_H);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_disabled(), 0);   /* 非本月的空格 */

        if (!valid) {
            lv_label_set_text(lb, "");
            continue;
        }

        snprintf(buf, sizeof(buf), "%d", day);
        lv_label_set_text(lb, buf);
        lv_obj_set_style_text_color(lb,
                                    (day == s_today) ? fw_theme_color_accent()
                                                     : fw_theme_color_text_primary(),
                                    0);
    }
}

static void today(void)
{
    const int64_t now = svc_time_now();
    struct tm tm = { 0 };
    time_t t = (time_t)now;
    localtime_r(&t, &tm);

    s_year = tm.tm_year + 1900;
    s_month = tm.tm_mon + 1;
    s_today = tm.tm_mday;
}

static void shift_month(int delta)
{
    int m = s_month + delta;
    int y = s_year;

    if (m < 1) {
        m = 12;
        y--;
    } else if (m > 12) {
        m = 1;
        y++;
    }

    s_month = m;
    s_year = y;
    s_today = 0;            /* 切走后不再标今天 */
    build_month();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    shift_month(-1);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    shift_month(1);
}

static void today_cb(lv_event_t *e)
{
    (void)e;
    today();
    build_month();
}

/* 顶部小按钮：左 / 右 / 今天 */
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 60, 28);
    lv_obj_align(btn, align, 0, 0);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
    return btn;
}

static void *calendar_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    today();

    if (!svc_time_is_synced()) {
        lv_obj_t *warn = lv_label_create(body);
        lv_label_set_text(warn, "时间未同步");
        lv_obj_set_style_text_font(warn, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(warn, fw_theme_color_text_secondary(), 0);
    }

    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_scrollable(bar, false);

    make_btn(bar, LV_SYMBOL_LEFT, LV_ALIGN_LEFT_MID, prev_cb);
    make_btn(bar, LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, next_cb);
    make_btn(bar, "今天", LV_ALIGN_CENTER, today_cb);

    s_title = lv_label_create(body);
    lv_obj_set_style_text_font(s_title, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_title, fw_theme_color_text_primary(), 0);

    s_grid = fw_ui_grid(body, WEEK_DAYS, CELL_W, CELL_H);

    build_month();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void calendar_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_title = NULL;
    s_grid = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_calendar_desc = {
    .name = "Calendar",
    .title = "日历",
    .icon_64 = &icon_home_calendar,
    .symbol = LV_SYMBOL_LIST,
    .on_create = calendar_on_create,
    .on_destroy = calendar_on_destroy,
};
