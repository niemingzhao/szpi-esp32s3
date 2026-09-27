/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Performance（性能监控）
 *
 * 两页，同一个根屏里显隐切换（返回键从任务页回概览）：
 *   概览：头部一行是「任务 N 个」入口，主信息卡是总 CPU 占用（24 px）与运行时长 / 复位原因，
 *         下面一张详情格放内部内存 / 历史最低 / PSRAM / Flash / TF 卡剩余 / 内置存储剩余；
 *   任务：一列任务（名字 + CPU% + 栈剩余），按名字排序、原地改数值（不重建列表，
 *         滚动位置不会被顶掉）。
 *
 * 数据来自 svc_sysinfo 与 svc_storage，每 2 s 刷新一次（任务 CPU 靠两次采样之差，
 * 第一次进来百分比都是 0）。App 不直接读 IDF。
 */

#include "app_perf.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.perf";

#define REFRESH_MS      2000
#define TASK_MAX        32
#define TASK_NAME_MAX   16      /* 与 svc_sysinfo_task_t.name 一致 */

/* 概览页的详情格（2 列 × 3 行，一格约 140 px，放得下"内部内存 + 45 KB"） */
static const char *const k_main_cells[6] = {
    "内部内存", "历史最低",
    "PSRAM", "Flash",
    "TF 卡剩", "内置剩",
};

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page_main = NULL;
static lv_obj_t *s_page_tasks = NULL;
static lv_obj_t *s_cpu_lb = NULL;
static lv_obj_t *s_sub_lb = NULL;
static lv_obj_t *s_table = NULL;
static lv_obj_t *s_task_lb = NULL;      /* 头部行"任务 N 个"入口里的文字 */
static lv_obj_t *s_task_list = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_foreground = false;
static bool s_on_tasks = false;

/* 任务列表：行按名字排序、建一次，之后只改数值（数量 / 名字变了才重建） */
static lv_obj_t *s_items[TASK_MAX];
static char (*s_names)[TASK_NAME_MAX] = NULL;
static size_t s_item_count = 0;

/* ------------------------------- 概览页 ------------------------------- */

static void uptime_text(uint32_t sec, char *out, size_t len)
{
    if (sec >= 3600) {
        snprintf(out, len, "%u 小时 %u 分", (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60));
    } else if (sec >= 60) {
        snprintf(out, len, "%u 分 %u 秒", (unsigned)(sec / 60), (unsigned)(sec % 60));
    } else {
        snprintf(out, len, "%u 秒", (unsigned)sec);
    }
}

static void refresh_main(const svc_sysinfo_t *si)
{
    char buf[80];

    if (s_cpu_lb != NULL) {
        uint8_t cpu = 0;
        if (svc_sysinfo_get_cpu_usage(&cpu) != ESP_OK) cpu = 0;
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)cpu);
        lv_label_set_text(s_cpu_lb, buf);
    }

    if (s_sub_lb != NULL) {
        char up[32];
        uptime_text(si->uptime_s, up, sizeof(up));
        snprintf(buf, sizeof(buf), "已运行 %s · 复位 %s", up,
                 si->reset_reason != NULL ? si->reset_reason : "-");
        lv_label_set_text(s_sub_lb, buf);
    }

    if (s_table == NULL) return;

    fw_ui_format_size(buf, sizeof(buf), si->heap_internal_free);
    fw_ui_table_value(s_table, 0, buf);

    fw_ui_format_size(buf, sizeof(buf), si->heap_internal_min);
    fw_ui_table_value(s_table, 1, buf);

    fw_ui_format_size(buf, sizeof(buf), si->heap_psram_free);
    fw_ui_table_value(s_table, 2, buf);

    fw_ui_format_size(buf, sizeof(buf), si->flash_size);
    fw_ui_table_value(s_table, 3, buf);

    /* 存储：未挂载（没插卡）时显示 —— */
    svc_storage_info_t info;
    if (svc_storage_get_info(SVC_STORAGE_TF_CARD, &info) == ESP_OK) {
        fw_ui_format_size(buf, sizeof(buf), info.free_bytes);
    } else {
        snprintf(buf, sizeof(buf), "未挂载");
    }
    fw_ui_table_value(s_table, 4, buf);

    if (svc_storage_get_info(SVC_STORAGE_INTERNAL_FLASH, &info) == ESP_OK) {
        fw_ui_format_size(buf, sizeof(buf), info.free_bytes);
    } else {
        snprintf(buf, sizeof(buf), "未挂载");
    }
    fw_ui_table_value(s_table, 5, buf);
}

/* ------------------------------- 任务页 ------------------------------- */

/* 按名字排序：顺序稳定，下面才能"原地改数值"而不是每次重建列表 */
static void sort_tasks(svc_sysinfo_task_t *tasks, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        svc_sysinfo_task_t key = tasks[i];
        size_t j = i;
        while (j > 0 && strncmp(tasks[j - 1].name, key.name, TASK_NAME_MAX) > 0) {
            tasks[j] = tasks[j - 1];
            j--;
        }
        tasks[j] = key;
    }
}

static void refresh_tasks(void)
{
    if (s_task_list == NULL || s_names == NULL) return;

    svc_sysinfo_task_t tasks[TASK_MAX];
    size_t n = 0;
    if (svc_sysinfo_get_tasks(tasks, TASK_MAX, &n) != ESP_OK) return;
    if (n > TASK_MAX) n = TASK_MAX;
    sort_tasks(tasks, n);

    /* 集合变了（任务建了 / 没了 / 名字不同）才重建；否则只改右侧数值，
     * 这样滚动位置不会被每 2 s 的刷新顶回顶部 */
    bool rebuild = (n != s_item_count);
    for (size_t i = 0; !rebuild && i < n; i++) {
        if (strncmp(s_names[i], tasks[i].name, TASK_NAME_MAX) != 0) rebuild = true;
    }

    if (rebuild) {
        lv_obj_clean(s_task_list);
        s_item_count = n;
        for (size_t i = 0; i < n; i++) {
            strlcpy(s_names[i], tasks[i].name, TASK_NAME_MAX);
            s_items[i] = fw_ui_list_add_value(s_task_list, tasks[i].name, "-", NULL, NULL);
        }
    }

    for (size_t i = 0; i < n; i++) {
        char buf[32];
        fw_ui_format_size(buf, sizeof(buf), tasks[i].stack_free);
        char value[64];                 /* 够放：CPU%（最长 10 位）+ " · 栈 " + 体积 + 结尾 */
        snprintf(value, sizeof(value), "%u%% · 栈 %s", (unsigned)tasks[i].cpu_percent, buf);
        if (s_items[i] != NULL) fw_ui_list_item_value(s_items[i], value);
    }

    if (s_task_lb != NULL) {
        char buf[24];
        snprintf(buf, sizeof(buf), "任务 %u 个", (unsigned)n);
        lv_label_set_text(s_task_lb, buf);
    }
}

/* ------------------------------- 刷新 / 翻页 ------------------------------- */

static void refresh(void)
{
    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) == ESP_OK) {
        refresh_main(&si);
    }
    refresh_tasks();
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

/* 只刷概览页（切回概览时用） */
static void refresh_main_only(void)
{
    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) == ESP_OK) refresh_main(&si);
}

static void show_page(bool tasks)
{
    s_on_tasks = tasks;
    if (s_page_main != NULL) lv_obj_set_hidden(s_page_main, tasks);
    if (s_page_tasks != NULL) lv_obj_set_hidden(s_page_tasks, !tasks);
    if (!tasks) refresh_main_only();
}

static void tasks_cb(lv_event_t *e)
{
    (void)e;
    refresh_tasks();            /* 进去之前先把列表和数量刷一遍 */
    show_page(true);
}

/* ------------------------------- 生命周期 ------------------------------- */

static void *perf_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);
    lv_obj_set_scrollable(body, false);

    s_foreground = false;
    s_on_tasks = false;
    s_item_count = 0;

    if (s_names == NULL) {
        s_names = malloc(sizeof(*s_names) * TASK_MAX);
    }

    /* ---- 概览页 ---- */
    s_page_main = lv_obj_create(body);
    lv_obj_set_width(s_page_main, lv_pct(100));
    lv_obj_set_flex_grow(s_page_main, 1);
    lv_obj_set_scrollable(s_page_main, false);
    lv_obj_set_style_bg_opa(s_page_main, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_main, 0, 0);
    lv_obj_set_style_pad_all(s_page_main, 0, 0);
    lv_obj_set_style_pad_row(s_page_main, 8, 0);
    lv_obj_set_flex_flow(s_page_main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page_main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 头部一行：任务入口（概览页的主角是 CPU，任务列表放第二页） */
    lv_obj_t *head = lv_obj_create(s_page_main);
    lv_obj_set_size(head, lv_pct(100), 28);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *task_btn = fw_ui_icon_btn(head, &icon_ui_more, "任务", 110, tasks_cb, NULL);
    s_task_lb = lv_obj_get_child(task_btn, 1);
    lv_obj_set_width(s_task_lb, 76);            /* 固定宽 + DOT：任务数变多也不会顶出按钮 */
    lv_label_set_long_mode(s_task_lb, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *card = fw_ui_hero_card(s_page_main, 64);

    lv_obj_t *line1 = lv_obj_create(card);
    lv_obj_set_size(line1, lv_pct(100), 26);
    lv_obj_set_scrollable(line1, false);
    lv_obj_set_style_bg_opa(line1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line1, 0, 0);
    lv_obj_set_style_pad_all(line1, 0, 0);
    lv_obj_set_flex_flow(line1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(line1, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cpu_lb = lv_label_create(line1);
    lv_label_set_text(s_cpu_lb, "0%");
    lv_obj_set_style_text_font(s_cpu_lb, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_cpu_lb, fw_theme_color_text_primary(), 0);

    lv_obj_t *cap = lv_label_create(line1);
    lv_label_set_text(cap, "总 CPU 占用");
    lv_obj_set_style_text_font(cap, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(cap, fw_theme_color_accent(), 0);

    s_sub_lb = lv_label_create(card);
    lv_obj_set_width(s_sub_lb, lv_pct(100));
    lv_label_set_long_mode(s_sub_lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_sub_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_sub_lb, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_sub_lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_table = fw_ui_table(s_page_main, 2, 3, k_main_cells);

    /* ---- 任务页 ---- */
    s_page_tasks = lv_obj_create(body);
    lv_obj_set_width(s_page_tasks, lv_pct(100));
    lv_obj_set_flex_grow(s_page_tasks, 1);
    lv_obj_set_scrollable(s_page_tasks, false);
    lv_obj_set_style_bg_opa(s_page_tasks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_tasks, 0, 0);
    lv_obj_set_style_pad_all(s_page_tasks, 0, 0);
    lv_obj_set_style_pad_row(s_page_tasks, 8, 0);
    lv_obj_set_flex_flow(s_page_tasks, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page_tasks, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_hidden(s_page_tasks, true);

    lv_obj_t *thead = lv_obj_create(s_page_tasks);
    lv_obj_set_size(thead, lv_pct(100), 28);
    lv_obj_set_scrollable(thead, false);
    lv_obj_set_style_bg_opa(thead, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(thead, 0, 0);
    lv_obj_set_style_pad_all(thead, 0, 0);
    lv_obj_set_flex_flow(thead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(thead, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 返回走状态栏返回键 / BOOT 单击（on_back 回概览页），页内不再放返回按钮 */
    lv_obj_t *title = lv_label_create(thead);
    lv_label_set_text(title, "任务列表");
    lv_obj_set_style_text_font(title, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);

    s_task_list = fw_ui_list(s_page_tasks, NULL);
    lv_obj_set_width(s_task_list, lv_pct(100));
    lv_obj_set_flex_grow(s_task_list, 1);

    refresh();

    /* 定时器先建好但不启用：等进入前台（on_start / on_resume）再恢复 */
    s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    if (s_timer != NULL) lv_timer_pause(s_timer);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* on_start 与 on_resume 都挂这个：换主题重建时后台 App 也会走 on_create，
 * 而暂停过的定时器在"首次进入"与"从返回栈回来"两条路径上都要恢复 */
static void perf_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh();
}

static void perf_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void perf_on_destroy(void *ctx)
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
    s_page_main = NULL;
    s_page_tasks = NULL;
    s_cpu_lb = NULL;
    s_sub_lb = NULL;
    s_table = NULL;
    s_task_lb = NULL;
    s_task_list = NULL;
    for (size_t i = 0; i < TASK_MAX; i++) s_items[i] = NULL;
    lvgl_port_unlock();

    free(s_names);
    s_names = NULL;
    s_item_count = 0;
    s_on_tasks = false;
}

/* 任务页开着时，返回键先回概览页 */
static bool perf_on_back(void *ctx)
{
    (void)ctx;

    if (!s_on_tasks) return false;

    show_page(false);
    return true;
}

const fw_app_desc_t app_perf_desc = {
    .name = "Perf",
    .title = "性能监控",
    .icon = &icon_home_perf,
    .on_create = perf_on_create,
    .on_start = perf_on_resume,
    .on_pause = perf_on_pause,
    .on_resume = perf_on_resume,
    .on_destroy = perf_on_destroy,
    .on_back = perf_on_back,
};
