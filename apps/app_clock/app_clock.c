/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Clock
 *
 * 演示 App 生命周期：on_create 建 UI 与定时器，on_pause/on_resume 暂停/恢复
 * 刷新，on_destroy 释放全部资源。时间来源为 svc_time（未同步时显示 --:--）。
 */

#include "app_clock.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "app.clock";

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time = NULL;
static lv_obj_t *s_date = NULL;
static lv_timer_t *s_timer = NULL;

static void refresh(void)
{
    if (s_time == NULL || s_date == NULL) return;

    if (!svc_time_is_synced()) {
        lv_label_set_text(s_time, "--:--");
        lv_label_set_text(s_date, "时间未同步");
        return;
    }

    int64_t now = svc_time_now();
    time_t t = (time_t)now;
    struct tm tmv;
    localtime_r(&t, &tmv);

    static const char *wd[] = { "日", "一", "二", "三", "四", "五", "六" };
    int w = (tmv.tm_wday >= 0 && tmv.tm_wday < 7) ? tmv.tm_wday : 0;

    char tb[8];
    char db[64];
    svc_time_format(now, "%H:%M", tb, sizeof(tb));
    snprintf(db, sizeof(db), "%04d-%02d-%02d 星期%s",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, wd[w]);

    lv_label_set_text(s_time, tb);
    lv_label_set_text(s_date, db);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void *clock_on_create(void)
{
    lvgl_port_lock(0);

    s_root = lv_obj_create(NULL);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_root, fw_theme_color_bg_primary(), 0);

    s_time = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_time, fw_asset_font_32(), 0);
    lv_obj_set_style_text_color(s_time, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, FW_STATUSBAR_H / 2 - 16);

    s_date = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_date, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_date, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, FW_STATUSBAR_H / 2 + 22);

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    refresh();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
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
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_time = NULL;
    s_date = NULL;
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
