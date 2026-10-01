/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Clock（APP-CLOCK 时钟）
 *
 * 大号时间（含秒，每秒刷新）+ 日期；并提供 12/24 小时制切换、手动校时
 * （加减方式，不引入文本输入）与时区预设切换。
 *
 * - 12/24 小时制持久化到 svc_settings 的 sys 命名空间（键 clock_24h）。
 * - 时区经 svc_time_set_timezone() 设置（该接口内部持久化到 sys/timezone）。
 * - 手动校时调 svc_time_set_manual()；未同步过时间时以 2026-01-01 00:00 UTC 为基准，
 *   避免从 1970 起步毫无意义。
 */

#include "app_clock.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "app.clock";

/* 设置存放：与 svc_time 一致走 sys 命名空间 */
#define CLK_SET_NS      "sys"
#define CLK_SET_24H     "clock_24h"

/* 手动校时：未同步时的基准（2026-01-01 00:00:00 UTC） */
#define CLK_BASE_TS     1767225600LL
#define CLK_BASE_MIN    1600000000LL

/* 常用时区预设（POSIX TZ 字符串，最后一个含夏令时规则） */
static const char *TZ_PRESETS[] = {
    "CST-8",
    "UTC",
    "JST-9",
    "PST8PDT",
    "CET-1CEST,M3.5.0,M10.5.0/3",
};
#define TZ_COUNT (sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]))

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time = NULL;
static lv_obj_t *s_date = NULL;
static lv_obj_t *s_fmt_row = NULL;
static lv_obj_t *s_tz_row = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_24h = true;
static char s_tz[32] = "CST-8";

static void section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_secondary(), 0);
}

static void refresh(void)
{
    if (s_time == NULL || s_date == NULL) return;

    if (!svc_time_is_synced()) {
        lv_label_set_text(s_time, "--:--:--");
        lv_label_set_text(s_date, "时间未同步");
        return;
    }

    const int64_t now = svc_time_now();
    const time_t t = (time_t)now;
    struct tm tmv;
    if (localtime_r(&t, &tmv) == NULL) {
        lv_label_set_text(s_time, "--:--:--");
        lv_label_set_text(s_date, "时间未同步");
        return;
    }

    static const char *wd[] = { "日", "一", "二", "三", "四", "五", "六" };
    const int w = (tmv.tm_wday >= 0 && tmv.tm_wday < 7) ? tmv.tm_wday : 0;

    char tb[20] = { 0 };
    char db[64] = { 0 };
    if (s_24h) {
        svc_time_format(now, "%H:%M:%S", tb, sizeof(tb));
    } else {
        char hm[12] = { 0 };
        svc_time_format(now, "%I:%M:%S", hm, sizeof(hm));
        snprintf(tb, sizeof(tb), "%s %s", hm, (tmv.tm_hour < 12) ? "AM" : "PM");
    }
    snprintf(db, sizeof(db), "%04d-%02d-%02d 星期%s",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, wd[w]);

    lv_label_set_text(s_time, tb);
    lv_label_set_text(s_date, db);
}

static void update_fmt_row(void)
{
    if (s_fmt_row != NULL) {
        fw_ui_row_btn_value(s_fmt_row, s_24h ? "24 小时" : "12 小时");
    }
}

static void update_tz_row(void)
{
    if (s_tz_row != NULL) {
        fw_ui_row_btn_value(s_tz_row, s_tz);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

/* 12/24 小时制切换（持久化） */
static void fmt_cb(lv_event_t *e)
{
    (void)e;

    s_24h = !s_24h;
    svc_settings_set_u8(CLK_SET_NS, CLK_SET_24H, s_24h ? 1 : 0);
    update_fmt_row();
    refresh();
}

/* 手动校时：user_data 为要加减的秒数（可为负） */
static void adjust_cb(lv_event_t *e)
{
    const int64_t delta = (int64_t)(intptr_t)lv_event_get_user_data(e);

    int64_t base = svc_time_now();
    if (!svc_time_is_synced() || base < CLK_BASE_MIN) {
        base = CLK_BASE_TS;
    }

    if (svc_time_set_manual(base + delta) == ESP_OK) {
        fw_ui_toast("时间已更新", 1500);
        refresh();
    } else {
        fw_ui_toast("设置失败", 1500);
    }
}

/* 切换时区：user_data 为预设字符串（静态存储期） */
static void tz_cb(lv_event_t *e)
{
    const char *tz = (const char *)lv_event_get_user_data(e);
    if (tz == NULL) return;

    if (svc_time_set_timezone(tz) != ESP_OK) {
        fw_ui_toast("设置失败", 1500);
        return;
    }

    strlcpy(s_tz, tz, sizeof(s_tz));
    update_tz_row();
    refresh();
    fw_ui_toast("时区已切换", 1500);
}

static void *clock_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 读取持久化设置 */
    uint8_t v24 = 1;
    svc_settings_get_u8(CLK_SET_NS, CLK_SET_24H, &v24, 1);
    s_24h = (v24 != 0);
    svc_settings_get_str(CLK_SET_NS, "timezone", s_tz, sizeof(s_tz), "CST-8");

    /* 大号时间 + 日期 */
    s_time = lv_label_create(body);
    lv_obj_set_width(s_time, lv_pct(100));
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_time, fw_asset_font_32(), 0);
    lv_obj_set_style_text_color(s_time, fw_theme_color_text_primary(), 0);

    s_date = lv_label_create(body);
    lv_obj_set_width(s_date, lv_pct(100));
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_date, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_date, fw_theme_color_text_secondary(), 0);

    s_fmt_row = fw_ui_row_btn(body, LV_SYMBOL_SETTINGS, "小时制", fmt_cb, NULL);
    update_fmt_row();

    section_label(body, "校时");
    fw_ui_row_btn(body, LV_SYMBOL_PLUS, "校时 +1 小时", adjust_cb, (void *)(intptr_t)3600);
    fw_ui_row_btn(body, LV_SYMBOL_MINUS, "校时 -1 小时", adjust_cb, (void *)(intptr_t)(-3600));
    fw_ui_row_btn(body, LV_SYMBOL_PLUS, "校时 +1 分", adjust_cb, (void *)(intptr_t)60);
    fw_ui_row_btn(body, LV_SYMBOL_MINUS, "校时 -1 分", adjust_cb, (void *)(intptr_t)(-60));

    s_tz_row = fw_ui_row_btn(body, LV_SYMBOL_GPS, "当前时区", NULL, NULL);
    update_tz_row();

    lv_obj_t *tz_list = fw_ui_list(body, "时区");
    for (size_t i = 0; i < TZ_COUNT; i++) {
        fw_ui_list_add(tz_list, TZ_PRESETS[i], tz_cb, (void *)TZ_PRESETS[i]);
    }

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (24h=%d tz=%s)", s_24h ? 1 : 0, s_tz);
    return s_root;
}

static void clock_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void clock_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh();
}

static void clock_on_destroy(void *ctx)
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
    s_time = NULL;
    s_date = NULL;
    s_fmt_row = NULL;
    s_tz_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_clock_desc = {
    .name = "Clock",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_BELL,
    .on_create = clock_on_create,
    .on_pause = clock_on_pause,
    .on_resume = clock_on_resume,
    .on_destroy = clock_on_destroy,
};
