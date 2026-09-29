/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Debug（APP-DEBUG 调试控制台）
 *
 * 只读面板：系统信息 / 内存与任务 CPU / 崩溃记录 / 最近日志（svc_sysinfo 的日志环）/
 * 已注册 App。每 2 秒刷新一次动态部分；崩溃记录可以清除。
 */

#include "app_debug.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.debug";

#define DBG_LOG_SHOW   512
#define DBG_TOP_TASKS  4

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_mem = NULL;
static lv_obj_t *s_tasks = NULL;
static lv_obj_t *s_crash = NULL;
static lv_obj_t *s_log = NULL;
static lv_obj_t *s_uptime_row = NULL;
static lv_timer_t *s_timer = NULL;

/* ------------------------------ 刷新 ------------------------------ */

static void refresh_system(void)
{
    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) != ESP_OK) return;

    if (s_uptime_row != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u s · %s", (unsigned)si.uptime_s, si.reset_reason);
        fw_ui_row_btn_value(s_uptime_row, buf);
    }

    if (s_mem != NULL) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "内部堆剩余 %u KB（历史最低 %u KB）\nPSRAM 剩余 %u KB",
                 (unsigned)(si.heap_internal_free / 1024),
                 (unsigned)(si.heap_internal_min / 1024),
                 (unsigned)(si.heap_psram_free / 1024));
        lv_label_set_text(s_mem, buf);
    }
}

static void refresh_tasks(void)
{
    if (s_tasks == NULL) return;

    svc_sysinfo_task_t tasks[32];
    size_t count = 0;
    if (svc_sysinfo_get_tasks(tasks, 32, &count) != ESP_OK) return;

    char buf[320];
    size_t used = 0;
    buf[0] = '\0';

    for (size_t i = 0; i < count && i < DBG_TOP_TASKS; i++) {
        int n = snprintf(buf + used, sizeof(buf) - used, "%s%s %u%% 栈 %u\n",
                         (i == 0) ? "" : "", tasks[i].name,
                         (unsigned)tasks[i].cpu_percent, (unsigned)tasks[i].stack_free);
        if (n <= 0 || (size_t)n >= sizeof(buf) - used) break;
        used += (size_t)n;
    }

    if (used == 0) {
        snprintf(buf, sizeof(buf), "暂无采样（再等一次刷新）");
    }
    lv_label_set_text(s_tasks, buf);
}

static void refresh_crash(void)
{
    if (s_crash == NULL) return;

    char buf[512];
    if (svc_sysinfo_get_crash_log(buf, sizeof(buf)) != ESP_OK) {
        lv_label_set_text(s_crash, "无记录");
        return;
    }
    lv_label_set_text(s_crash, buf);
}

static void refresh_log(void)
{
    if (s_log == NULL) return;

    static char buf[DBG_LOG_SHOW + 4];   /* 512 B 静态缓冲，不占堆 */
    if (svc_sysinfo_get_recent_logs(buf, sizeof(buf)) != ESP_OK) {
        lv_label_set_text(s_log, "(还没有日志)");
        return;
    }

    /* 只更新文本：不再每次都 lv_obj_scroll_to_view()，否则 512 B 的自动换行标签
     * 会把 LVGL 任务拖到看门狗超时（现场：task_wdt abort，回溯停在 draw_scrollbar →
     * lv_obj_get_self_height → 字体 glyph 查询） */
    lv_label_set_text(s_log, buf);
}

static void refresh_all(void)
{
    refresh_system();
    refresh_tasks();
    refresh_log();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_all();
}

/* ------------------------------ 交互 ------------------------------ */

static void clear_crash_cb(lv_event_t *e)
{
    (void)e;

    if (svc_sysinfo_clear_crash_log() == ESP_OK) {
        fw_ui_toast("崩溃记录已清除", 1500);
    } else {
        fw_ui_toast("清除失败", 1500);
    }
    refresh_crash();
}

/* ------------------------------ 生命周期 ------------------------------ */

static lv_obj_t *caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    return l;
}

static lv_obj_t *mono(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_secondary(), 0);
    return l;
}

static void *debug_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    svc_sysinfo_t si;
    memset(&si, 0, sizeof(si));
    svc_sysinfo_get(&si);

    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s · %s", si.project_name, SZPI_OS_VERSION, si.chip_model);
    lv_obj_t *head = lv_label_create(body);
    lv_label_set_text(head, buf);
    lv_obj_set_style_text_font(head, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(head, fw_theme_color_accent(), 0);

    s_uptime_row = fw_ui_row_btn(body, LV_SYMBOL_PLAY, "运行 / 复位原因", NULL, NULL);
    snprintf(buf, sizeof(buf), "IDF %s · MAC %s", si.idf_version, si.mac);
    fw_ui_row_btn_value(s_uptime_row, buf);

    caption(body, "内存");
    s_mem = mono(body, "");

    caption(body, "任务（CPU / 栈剩余）");
    s_tasks = mono(body, "");

    caption(body, "崩溃记录");
    s_crash = mono(body, "");

    lv_obj_t *clear = fw_ui_row_btn(body, LV_SYMBOL_TRASH, "清除崩溃记录", clear_crash_cb, NULL);
    (void)clear;

    caption(body, "最近日志");
    s_log = mono(body, "");

    caption(body, "已注册 App");
    const fw_app_desc_t *apps[24];
    size_t n = fw_app_mgr_list(apps, 24);
    char names[192];
    names[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        strlcat(names, apps[i]->name, sizeof(names));
        if (i + 1 < n) strlcat(names, " · ", sizeof(names));
    }
    snprintf(buf, sizeof(buf), "%u 个：%.100s", (unsigned)n, names);
    mono(body, buf);

    refresh_all();
    refresh_crash();

    s_timer = lv_timer_create(timer_cb, 3000, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void debug_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void debug_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh_all();
    refresh_crash();
}

static void debug_on_destroy(void *ctx)
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
    s_mem = NULL;
    s_tasks = NULL;
    s_crash = NULL;
    s_log = NULL;
    s_uptime_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_debug_desc = {
    .name = "Debug",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_WARNING,
    .on_create = debug_on_create,
    .on_pause = debug_on_pause,
    .on_resume = debug_on_resume,
    .on_destroy = debug_on_destroy,
};
