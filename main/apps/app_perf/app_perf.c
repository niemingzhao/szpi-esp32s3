/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Performance（APP-PERF 性能监控）
 *
 * 总 CPU 占用 + 内存余量 + Flash 容量 + 存储占用（TF 卡 / 内置）+ 任务快照
 * （CPU% / 栈剩余）。数据来自 svc_sysinfo 与 svc_storage，每 2 s 刷新一次
 * （任务 CPU 靠两次采样之差，第一次进来百分比都是 0）。
 */

#include "app_perf.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "app.perf";

#define REFRESH_MS      2000
#define TASK_MAX        32

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_cpu_row = NULL;
static lv_obj_t *s_mem_row = NULL;
static lv_obj_t *s_min_row = NULL;
static lv_obj_t *s_psram_row = NULL;
static lv_obj_t *s_uptime_row = NULL;
static lv_obj_t *s_flash_row = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_store_list = NULL;
static lv_timer_t *s_timer = NULL;

static void refresh_storage(void)
{
    if (s_store_list == NULL) return;

    lv_obj_clean(s_store_list);

    periph_storage_info_t info;
    char line[96];

    /* TF 卡：未插入 / 已拔出时 get_info 返回错误，按"未挂载"显示，不算失败 */
    if (svc_storage_get_info(PERIPH_STORAGE_TF_CARD, &info) == ESP_OK) {
        snprintf(line, sizeof(line), "TF 卡  已用 %u / 总 %u KB  剩 %u KB",
                 (unsigned)((info.total_bytes - info.free_bytes) / 1024),
                 (unsigned)(info.total_bytes / 1024),
                 (unsigned)(info.free_bytes / 1024));
    } else {
        snprintf(line, sizeof(line), "TF 卡  未挂载");
    }
    fw_ui_list_add(s_store_list, line, NULL, NULL);

    if (svc_storage_get_info(PERIPH_STORAGE_INTERNAL_FLASH, &info) == ESP_OK) {
        snprintf(line, sizeof(line), "内置存储  已用 %u / 总 %u KB  剩 %u KB",
                 (unsigned)((info.total_bytes - info.free_bytes) / 1024),
                 (unsigned)(info.total_bytes / 1024),
                 (unsigned)(info.free_bytes / 1024));
    } else {
        snprintf(line, sizeof(line), "内置存储  未挂载");
    }
    fw_ui_list_add(s_store_list, line, NULL, NULL);
}

static void refresh(void)
{
    char buf[64];

    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(si.heap_internal_free / 1024));
        if (s_mem_row != NULL) fw_ui_row_btn_value(s_mem_row, buf);

        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(si.heap_internal_min / 1024));
        if (s_min_row != NULL) fw_ui_row_btn_value(s_min_row, buf);

        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(si.heap_psram_free / 1024));
        if (s_psram_row != NULL) fw_ui_row_btn_value(s_psram_row, buf);

        snprintf(buf, sizeof(buf), "%u 秒", (unsigned)si.uptime_s);
        if (s_uptime_row != NULL) fw_ui_row_btn_value(s_uptime_row, buf);

        snprintf(buf, sizeof(buf), "%u KB", (unsigned)(si.flash_size / 1024));
        if (s_flash_row != NULL) fw_ui_row_btn_value(s_flash_row, buf);
    }

    uint8_t cpu = 0;
    if (svc_sysinfo_get_cpu_usage(&cpu) == ESP_OK && s_cpu_row != NULL) {
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)cpu);
        fw_ui_row_btn_value(s_cpu_row, buf);
    }

    refresh_storage();

    svc_sysinfo_task_t tasks[TASK_MAX];
    size_t n = 0;
    if (svc_sysinfo_get_tasks(tasks, TASK_MAX, &n) != ESP_OK || s_list == NULL) return;

    lv_obj_clean(s_list);

    char line[72];
    for (size_t i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "%s  %u%%  栈 %u",
                 tasks[i].name, (unsigned)tasks[i].cpu_percent, (unsigned)tasks[i].stack_free);
        fw_ui_list_add(s_list, line, NULL, NULL);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void *perf_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_cpu_row = fw_ui_row_btn(body, LV_SYMBOL_PLAY, "总 CPU", NULL, NULL);
    s_mem_row = fw_ui_row_btn(body, LV_SYMBOL_DOWNLOAD, "内部内存 空闲", NULL, NULL);
    s_min_row = fw_ui_row_btn(body, LV_SYMBOL_WARNING, "内部内存 历史最低", NULL, NULL);
    s_psram_row = fw_ui_row_btn(body, LV_SYMBOL_DRIVE, "PSRAM 空闲", NULL, NULL);
    s_uptime_row = fw_ui_row_btn(body, LV_SYMBOL_REFRESH, "运行时长", NULL, NULL);
    s_flash_row = fw_ui_row_btn(body, LV_SYMBOL_DRIVE, "Flash 容量", NULL, NULL);

    s_store_list = fw_ui_list(body, "存储");
    s_list = fw_ui_list(body, "任务");

    refresh();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void perf_on_start(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer == NULL) {
        s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    }
    lvgl_port_unlock();
}

static void perf_on_pause(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    lvgl_port_unlock();
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
    s_cpu_row = NULL;
    s_mem_row = NULL;
    s_min_row = NULL;
    s_psram_row = NULL;
    s_uptime_row = NULL;
    s_flash_row = NULL;
    s_list = NULL;
    s_store_list = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_perf_desc = {
    .name = "Perf",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_LOOP,
    .on_create = perf_on_create,
    .on_start = perf_on_start,
    .on_pause = perf_on_pause,
    .on_destroy = perf_on_destroy,
};
