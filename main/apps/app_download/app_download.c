/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Download（APP-DOWNLOAD 下载器）
 *
 * 输入 http(s) 链接，流式下载到 TF 卡根目录（文件名取 URL 最后一段；.lua 存到脚本目录）。
 * 用 svc_http_download() 的进度回调驱动进度条；回调在 svc.http 任务里执行，
 * 碰 LVGL 必须加锁，且按百分比变化节流（长度未知的分块响应退回按 KB 提示），
 * 避免每个 chunk 都刷界面。
 */

#include "app_download.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.download";

#define KB_HEIGHT        130

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_url_row = NULL;
static lv_obj_t *s_status_row = NULL;
static lv_obj_t *s_bar = NULL;          /* fw_ui_progress_bar() 返回的面板 */

/* 链接输入浮层 */
static lv_obj_t *s_input = NULL;
static lv_obj_t *s_input_ta = NULL;

static char s_url[192] = { 0 };
static char s_saved_path[224] = { 0 };
static bool s_busy = false;
static uint8_t s_last_pct = 0xFF;       /* 已刷到界面的百分比，用于节流 */
static size_t s_last_kb = 0;            /* total 未知时按 KB 节流 */

static void url_basename(const char *url, char *out, size_t len)
{
    const char *q = strchr(url, '?');
    const size_t n = q ? (size_t)(q - url) : strlen(url);

    const char *slash = NULL;
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '/') slash = url + i;
    }

    const char *name = slash ? slash + 1 : url;
    if (slash == NULL || (size_t)(name - url) >= n || *name == '\0') {
        snprintf(out, len, "download.bin");
        return;
    }

    size_t m = n - (size_t)(name - url);
    if (m >= len) m = len - 1;
    memcpy(out, name, m);
    out[m] = '\0';
}

/* 脚本（.lua）放到脚本目录，其余文件放 TF 卡根目录 */
static void save_path_of(const char *name, char *out, size_t len)
{
    const size_t n = strlen(name);

    if (n >= 4 && strcmp(name + n - 4, ".lua") == 0) {
        svc_storage_mkdir(FW_SCRIPT_DIR);       /* 已存在会失败，忽略 */
        snprintf(out, len, "%s/%s", FW_SCRIPT_DIR, name);
    } else {
        snprintf(out, len, "/sdcard/%s", name);
    }
}

/* 进度 / 结束回调（在 svc.http 任务里执行，不是 LVGL 任务） */
static void dl_progress(size_t received, size_t total, esp_err_t err, bool done, void *user)
{
    (void)user;

    if (!done) {
        /* 传输中：进度条按百分比节流；total 未知（分块传输）时退回按 KB 提示 */
        if (total == 0) {
            const size_t kb = received / 1024;
            if (kb == s_last_kb) return;
            s_last_kb = kb;

            char txt[32];
            snprintf(txt, sizeof(txt), "已下载 %u KB", (unsigned)kb);
            lvgl_port_lock(0);
            if (s_status_row != NULL) {
                fw_ui_row_btn_value(s_status_row, txt);
            }
            lvgl_port_unlock();
            return;
        }

        size_t pct = (size_t)(((uint64_t)received * 100u) / total);
        if (pct > 100) pct = 100;
        if ((uint8_t)pct == s_last_pct) return;
        s_last_pct = (uint8_t)pct;

        /* 回调在 svc.http 任务里：加锁 + 判空，避免 App 已退出时用到悬空对象 */
        lvgl_port_lock(0);
        if (s_bar != NULL) {
            fw_ui_progress_set(s_bar, (uint8_t)pct);
        }
        lvgl_port_unlock();
        return;
    }

    /* 结束：可能在别的任务里，碰界面统一加锁 */
    lvgl_port_lock(0);
    s_busy = false;
    if (s_bar != NULL) {
        fw_ui_progress_set(s_bar, (err == ESP_OK) ? 100 : 0);
    }
    if (s_status_row != NULL) {
        fw_ui_row_btn_value(s_status_row, (err == ESP_OK) ? s_saved_path : "失败");
    }
    lvgl_port_unlock();

    fw_ui_toast((err == ESP_OK) ? "下载完成" : "下载失败", 2500);
}

static void start_download(void)
{
    if (s_busy) {
        fw_ui_toast("正在下载…", 1500);
        return;
    }
    if (s_url[0] == '\0') {
        fw_ui_toast("请先输入链接", 2000);
        return;
    }

    char name[128] = { 0 };
    url_basename(s_url, name, sizeof(name));
    save_path_of(name, s_saved_path, sizeof(s_saved_path));

    s_busy = true;
    s_last_pct = 0xFF;
    s_last_kb = 0;
    if (s_bar != NULL) {
        lv_obj_set_hidden(s_bar, false);
        fw_ui_progress_set(s_bar, 0);
    }
    fw_ui_row_btn_value(s_status_row, "下载中…");

    /* 回调可能很快就到，先置好 busy / 路径再启动 */
    if (svc_http_download(s_url, s_saved_path, dl_progress, NULL) != ESP_OK) {
        s_busy = false;
        fw_ui_row_btn_value(s_status_row, "失败");
        fw_ui_toast("启动下载失败", 2000);
    }
}

/* ------------------------------- 链接输入 ------------------------------- */

static void input_close(void)
{
    if (s_input != NULL) {
        lv_obj_delete(s_input);
        s_input = NULL;
        s_input_ta = NULL;
    }
}

static void kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        if (s_input_ta != NULL) {
            snprintf(s_url, sizeof(s_url), "%s", lv_textarea_get_text(s_input_ta));
            fw_ui_row_btn_value(s_url_row, s_url);
        }
        input_close();
    } else if (code == LV_EVENT_CANCEL) {
        input_close();
    }
}

static void input_open(void)
{
    if (s_input != NULL) return;

    s_input = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input);
    lv_obj_set_size(s_input, lv_pct(100), lv_pct(100));
    lv_obj_center(s_input);
    lv_obj_set_style_bg_color(s_input, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_input, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_input, 8, 0);
    lv_obj_set_scrollable(s_input, false);

    lv_obj_t *lb = lv_label_create(s_input);
    lv_label_set_text(lb, "下载链接");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 0, 0);

    s_input_ta = lv_textarea_create(s_input);
    lv_textarea_set_one_line(s_input_ta, true);
    lv_textarea_set_placeholder_text(s_input_ta, "http://…");
    lv_obj_set_size(s_input_ta, lv_pct(100), 36);
    lv_obj_align(s_input_ta, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_font(s_input_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_input_ta, fw_theme_color_text_primary(), 0);

    lv_obj_t *kb = lv_keyboard_create(s_input);
    lv_obj_set_size(kb, lv_pct(100), KB_HEIGHT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_input_ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------------------------------- 事件 ---------------------------------- */

static void url_cb(lv_event_t *e)
{
    (void)e;
    input_open();
}

static void dl_cb(lv_event_t *e)
{
    (void)e;
    start_download();
}

static void *download_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_url_row = fw_ui_row_btn(body, LV_SYMBOL_EDIT, "链接", url_cb, NULL);
    fw_ui_row_btn_value(s_url_row, s_url[0] ? s_url : "(未输入)");

    fw_ui_row_btn(body, LV_SYMBOL_DOWNLOAD, "开始下载", dl_cb, NULL);

    s_status_row = fw_ui_row_btn(body, LV_SYMBOL_SAVE, "结果", NULL, NULL);

    s_bar = fw_ui_progress_bar(body, "下载进度");
    if (s_bar != NULL) {
        lv_obj_set_hidden(s_bar, true);     /* 空闲时隐藏，下载中才显示 */
        fw_ui_progress_set(s_bar, 0);
    }
    s_last_pct = 0xFF;
    s_last_kb = 0;

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void download_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_input = NULL;                 /* 随根屏一起删掉，避免悬空 */
    s_input_ta = NULL;
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_url_row = NULL;
    s_status_row = NULL;
    s_bar = NULL;
    lvgl_port_unlock();

    s_busy = false;
    s_last_pct = 0xFF;
    s_last_kb = 0;
}

const fw_app_desc_t app_download_desc = {
    .name = "Download",
    .title = "下载器",
    .icon_64 = &icon_home_download,
    .symbol = LV_SYMBOL_DOWNLOAD,
    .on_create = download_on_create,
    .on_destroy = download_on_destroy,
};
