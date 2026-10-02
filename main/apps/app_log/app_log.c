/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Log（APP-LOG 系统日志）
 *
 * 显示最近日志（无锁环形的尾部）与上一次崩溃现场，数据来自 svc_sysinfo。
 * 支持按级别筛选（全部 / 错误+警告 / 仅错误）：svc_sysinfo_get_recent_logs() 返回的是
 * 纯文本，所以只能按行首的级别字符（`E `/`W `/`I `/`D `）粗筛，续行会一并略去。
 * 按 AGENTS 4.23 的约定：只显示最近 512 B、刷新间隔 3 s、不自动滚动
 * （超大的自动换行标签会让 LVGL 任务长时间卡在字形布局上，饿死同核任务）。
 */

#include "app_log.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.log";

#define LOG_SHOW_MAX    512
#define REFRESH_MS      3000

/* 级别筛选：0 = 全部，1 = 错误 + 警告，2 = 仅错误 */
static const char *const k_filter_names[] = { "全部", "错误+警告", "仅错误" };
#define FILTER_COUNT    (sizeof(k_filter_names) / sizeof(k_filter_names[0]))

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_log_lb = NULL;
static lv_obj_t *s_filter_row = NULL;
static lv_timer_t *s_timer = NULL;

static unsigned s_filter = 0;

static void show_text(const char *text)
{
    if (s_log_lb != NULL) {
        lv_label_set_text(s_log_lb, text);
    }
}

static void show_filter(void)
{
    if (s_filter_row != NULL) {
        fw_ui_row_btn_value(s_filter_row, k_filter_names[s_filter]);
    }
}

/* 行是否命中当前级别筛选（按行首的级别字符粗筛，见文件头说明） */
static bool line_matches(const char *line, size_t len)
{
    if (s_filter == 0) return true;
    if (len < 2 || line[1] != ' ') return false;

    if (s_filter == 2) return line[0] == 'E';
    return line[0] == 'E' || line[0] == 'W';
}

static void refresh(void)
{
    char raw[LOG_SHOW_MAX + 1];

    if (svc_sysinfo_get_recent_logs(raw, sizeof(raw)) != ESP_OK) {
        show_text("(暂无日志)");
        return;
    }
    if (s_filter == 0) {
        show_text(raw);
        return;
    }

    /* 过滤结果放堆上：本函数在 LVGL 定时器里跑，避免再占 512 B 栈 */
    char *out = malloc(LOG_SHOW_MAX + 1);
    if (out == NULL) {
        show_text(raw);
        return;
    }

    size_t o = 0;
    const char *p = raw;

    while (*p != '\0' && o + 1 < (LOG_SHOW_MAX + 1)) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);

        if (line_matches(p, len)) {
            size_t copy = len;
            if (copy > LOG_SHOW_MAX - o) copy = LOG_SHOW_MAX - o;
            memcpy(out + o, p, copy);
            o += copy;
            if (o + 1 < (LOG_SHOW_MAX + 1)) out[o++] = '\n';
        }

        if (nl == NULL) break;
        p = nl + 1;
    }
    out[o] = '\0';

    show_text((o > 0) ? out : "(没有匹配的日志)");
    free(out);
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    refresh();
}

static void filter_cb(lv_event_t *e)
{
    (void)e;

    s_filter = (s_filter + 1) % FILTER_COUNT;
    show_filter();
    refresh();
}

static void crash_cb(lv_event_t *e)
{
    (void)e;

    char buf[LOG_SHOW_MAX + 1];
    if (svc_sysinfo_get_crash_log(buf, sizeof(buf)) == ESP_OK) {
        show_text(buf);
    } else {
        show_text("(没有崩溃记录)");
    }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;

    svc_sysinfo_clear_crash_log();
    fw_ui_toast("已清除崩溃记录", 2000);
    refresh();
}

static void *log_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    fw_ui_row_btn(body, LV_SYMBOL_REFRESH, "刷新日志", refresh_btn_cb, NULL);

    s_filter_row = fw_ui_row_btn(body, LV_SYMBOL_BARS, "过滤", filter_cb, NULL);
    show_filter();

    fw_ui_row_btn(body, LV_SYMBOL_WARNING, "崩溃记录", crash_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_TRASH, "清除崩溃记录", clear_cb, NULL);

    /* 固定高度的滚动容器：长日志在这里手动滚动，不带动整个页面 */
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, 110);
    lv_obj_set_style_bg_color(box, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(box, 8, 0);

    s_log_lb = lv_label_create(box);
    lv_obj_set_width(s_log_lb, lv_pct(100));
    lv_label_set_long_mode(s_log_lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_log_lb, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_log_lb, fw_theme_color_text_secondary(), 0);
    lv_label_set_text(s_log_lb, "(读取中)");

    refresh();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void log_on_start(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer == NULL) {
        s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    }
    lvgl_port_unlock();
}

static void log_on_pause(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lvgl_port_unlock();
}

static void log_on_destroy(void *ctx)
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
    s_log_lb = NULL;
    s_filter_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_log_desc = {
    .name = "Log",
    .title = "系统日志",
    .icon_64 = &icon_home_log,
    .symbol = LV_SYMBOL_LIST,
    .on_create = log_on_create,
    .on_start = log_on_start,
    .on_pause = log_on_pause,
    .on_destroy = log_on_destroy,
};
