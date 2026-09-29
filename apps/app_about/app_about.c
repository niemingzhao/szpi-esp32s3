/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - About（APP-ABOUT 关于本机）
 *
 * 只读信息页：项目 / 版本 / 编译时间 / 芯片 / MAC / Flash / IDF / 内存 / 运行时长，
 * 以及开源协议说明。数据全部来自 svc_sysinfo（App 不直接读 IDF）。
 */

#include "app_about.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.about";

static lv_obj_t *s_root = NULL;

/* 需要动态刷新的两行 */
static lv_obj_t *s_heap_row = NULL;
static lv_obj_t *s_uptime_row = NULL;

static void fmt_kb(char *buf, size_t len, size_t bytes)
{
    snprintf(buf, len, "%u KB", (unsigned)(bytes / 1024));
}

static void refresh(void)
{
    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) != ESP_OK) return;

    if (s_heap_row != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "内 %u KB / PSRAM %u KB",
                 (unsigned)(si.heap_internal_free / 1024),
                 (unsigned)(si.heap_psram_free / 1024));
        fw_ui_row_btn_value(s_heap_row, buf);
    }

    if (s_uptime_row != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%u 秒", (unsigned)si.uptime_s);
        fw_ui_row_btn_value(s_uptime_row, buf);
    }
}

static void *about_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    svc_sysinfo_t si;
    memset(&si, 0, sizeof(si));
    svc_sysinfo_get(&si);

    char buf[96];

    lv_obj_t *row = fw_ui_row_btn(body, LV_SYMBOL_FILE, "项目", NULL, NULL);
    fw_ui_row_btn_value(row, si.project_name);

    row = fw_ui_row_btn(body, LV_SYMBOL_OK, "固件版本", NULL, NULL);
    fw_ui_row_btn_value(row, SZPI_OS_VERSION);

    row = fw_ui_row_btn(body, LV_SYMBOL_UPLOAD, "应用版本", NULL, NULL);
    fw_ui_row_btn_value(row, si.app_version);

    row = fw_ui_row_btn(body, LV_SYMBOL_EDIT, "编译时间", NULL, NULL);
    snprintf(buf, sizeof(buf), "%s %s", si.build_date, si.build_time);
    fw_ui_row_btn_value(row, buf);

    row = fw_ui_row_btn(body, LV_SYMBOL_LIST, "芯片", NULL, NULL);
    snprintf(buf, sizeof(buf), "%s ×%u rev %u", si.chip_model,
             (unsigned)si.chip_cores, (unsigned)si.chip_revision);
    fw_ui_row_btn_value(row, buf);

    row = fw_ui_row_btn(body, LV_SYMBOL_WIFI, "MAC", NULL, NULL);
    fw_ui_row_btn_value(row, si.mac);

    row = fw_ui_row_btn(body, LV_SYMBOL_DRIVE, "Flash", NULL, NULL);
    fmt_kb(buf, sizeof(buf), si.flash_size);
    fw_ui_row_btn_value(row, buf);

    row = fw_ui_row_btn(body, LV_SYMBOL_SETTINGS, "ESP-IDF", NULL, NULL);
    fw_ui_row_btn_value(row, si.idf_version);

    s_heap_row = fw_ui_row_btn(body, LV_SYMBOL_DOWNLOAD, "空闲内存", NULL, NULL);
    s_uptime_row = fw_ui_row_btn(body, LV_SYMBOL_PLAY, "运行时长", NULL, NULL);

    lv_obj_t *lic = lv_label_create(body);
    lv_label_set_text(lic, "开源协议：Noto Sans SC (OFL) · LVGL (MIT) · ESP-IDF (Apache-2.0)");
    lv_obj_set_width(lic, lv_pct(100));
    lv_label_set_long_mode(lic, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lic, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lic, fw_theme_color_text_secondary(), 0);

    refresh();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void about_on_resume(void *ctx)
{
    (void)ctx;
    lvgl_port_lock(0);
    refresh();
    lvgl_port_unlock();
}

static void about_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_heap_row = NULL;
    s_uptime_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_about_desc = {
    .name = "About",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_FILE,
    .on_create = about_on_create,
    .on_resume = about_on_resume,
    .on_destroy = about_on_destroy,
};
