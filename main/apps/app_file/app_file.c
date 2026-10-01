/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - File（APP-FILE 文件管理器）
 *
 * 浏览 TF 卡（/sdcard）与内置 SPIFFS（/internal）：目录可进可退，文件按类型显示图标
 * 与大小；点文件跳到对应 App（音频→Music，图片→Image，文本→Editor，其它给提示）。
 * PRD 第一版只要求浏览，所以不做删除 / 重命名。
 */

#include "app_file.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "app.file";

#define FILE_ROW_MAX   16
#define FILE_ROW_LEN   64
#define FILE_PATH_MAX  128
#define FORMAT_STACK   6144

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_path = NULL;

static char s_cur[FILE_PATH_MAX] = "/sdcard";   /* 当前目录 */
/* 目录项放堆上（FILE_ROW_MAX × FILE_ROW_LEN = 1 KB），不在 App 里放大静态数组（见 AGENTS 4.20） */
static char (*s_rows)[FILE_ROW_LEN] = NULL;
static bool *s_row_dir = NULL;
static size_t s_row_count = 0;

/* row_cb 里进目录后要刷新列表，所以先声明 */
static void scan(void);

/* ------------------------------- 工具 ------------------------------- */

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return (dot != NULL) && (strcasecmp(dot, ext) == 0);
}

static const char *icon_for(const char *name)
{
    if (has_ext(name, ".mp3") || has_ext(name, ".wav")) return LV_SYMBOL_AUDIO;
    if (has_ext(name, ".png") || has_ext(name, ".gif") || has_ext(name, ".bmp") ||
        has_ext(name, ".jpg")) return LV_SYMBOL_IMAGE;
    if (has_ext(name, ".avi") || has_ext(name, ".mjpeg")) return LV_SYMBOL_VIDEO;
    if (has_ext(name, ".txt")) return LV_SYMBOL_EDIT;
    if (has_ext(name, ".bin")) return LV_SYMBOL_DOWNLOAD;
    return LV_SYMBOL_FILE;
}

static void full_path(size_t idx, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", s_cur, s_rows[idx]);
}

static bool is_root_dir(void)
{
    return (strcmp(s_cur, "/sdcard") == 0) || (strcmp(s_cur, "/internal") == 0);
}

static void go_up(void)
{
    if (is_root_dir()) return;

    char *slash = strrchr(s_cur, '/');
    if (slash != NULL && slash != s_cur) {
        *slash = '\0';
    }
    scan();
}

static void open_with_app(const char *path)
{
    const char *name = strrchr(path, '/');
    name = (name != NULL) ? name + 1 : path;

    char uri[FILE_PATH_MAX + FILE_ROW_LEN + 16];
    const char *target = NULL;

    if (has_ext(name, ".mp3") || has_ext(name, ".wav")) {
        snprintf(uri, sizeof(uri), "Music?path=%s", path);
        target = uri;
    } else if (has_ext(name, ".png") || has_ext(name, ".gif") || has_ext(name, ".bmp")) {
        target = "Image";
    } else if (has_ext(name, ".txt")) {
        target = "Editor";
    }

    if (target == NULL) {
        fw_ui_toast("没有关联的应用，请在 PC 上打开", 2500);
        return;
    }

    if (fw_app_mgr_launch_uri(target) != ESP_OK) {
        fw_ui_toast("启动失败", 1500);
    }
}

/* ------------------------------- 列表 ------------------------------- */

static void row_cb(lv_event_t *e)
{
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_row_count) return;

    char path[FILE_PATH_MAX + FILE_ROW_LEN];
    full_path(idx, path, sizeof(path));

    if (s_row_dir[idx]) {
        strlcpy(s_cur, path, sizeof(s_cur));
        scan();
        return;
    }

    open_with_app(path);
}

static void scan(void)
{
    s_row_count = 0;

    if (s_rows == NULL || s_row_dir == NULL) return;    /* 内存不足：on_create 已提示 */

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(s_cur, &it) == ESP_OK) {
        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL) {
            if (s_row_count >= FILE_ROW_MAX) break;
            strlcpy(s_rows[s_row_count], entry->name, FILE_ROW_LEN);
            s_row_dir[s_row_count] = entry->is_dir;
            s_row_count++;
        }
        svc_storage_iter_end(it);
    }

    if (s_path != NULL) {
        lv_label_set_text(s_path, s_cur);
    }

    if (s_list != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_list);
        if (s_row_count == 0) {
            fw_ui_list_add(s_list, "空目录", NULL, NULL);
        } else {
            for (size_t i = 0; i < s_row_count; i++) {
                char line[FILE_ROW_LEN + 32];
                char path[FILE_PATH_MAX + FILE_ROW_LEN];

                if (s_row_dir[i]) {
                    snprintf(line, sizeof(line), "%s  %s", LV_SYMBOL_DIRECTORY, s_rows[i]);
                } else {
                    full_path(i, path, sizeof(path));
                    size_t size = 0;
                    svc_storage_exists(path, &size);
                    snprintf(line, sizeof(line), "%s  %s   %u KB", icon_for(s_rows[i]), s_rows[i],
                             (unsigned)((size + 1023) / 1024));
                }
                fw_ui_list_add(s_list, line, row_cb, (void *)(intptr_t)i);
            }
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "%s: %u entr(ies)", s_cur, (unsigned)s_row_count);
}

/* ------------------------------ 交互 ------------------------------ */

static void tf_cb(lv_event_t *e)
{
    (void)e;
    strlcpy(s_cur, "/sdcard", sizeof(s_cur));
    scan();
}

static void internal_cb(lv_event_t *e)
{
    (void)e;
    strlcpy(s_cur, "/internal", sizeof(s_cur));
    scan();
}

static void up_cb(lv_event_t *e)
{
    (void)e;
    if (is_root_dir()) {
        fw_ui_toast("已经在根目录", 1500);
        return;
    }
    go_up();
}

/* ------------------------------ 格式化 ------------------------------ */

static bool s_formatting = false;
static bool s_fmt_ok = false;
static periph_storage_type_t s_fmt_type = PERIPH_STORAGE_INTERNAL_FLASH;

static void format_done_async(void *arg)
{
    (void)arg;

    lvgl_port_lock(0);
    fw_ui_toast(s_fmt_ok ? "格式化完成" : "格式化失败", 2000);
    strlcpy(s_cur, (s_fmt_type == PERIPH_STORAGE_TF_CARD) ? "/sdcard" : "/internal",
            sizeof(s_cur));
    scan();
    lvgl_port_unlock();

    svc_watchdog_arm();
    s_formatting = false;
}

static void format_task(void *arg)
{
    const periph_storage_type_t type = (periph_storage_type_t)(intptr_t)arg;
    s_fmt_ok = (svc_storage_format(type) == ESP_OK);

    /* 回到 LVGL 任务刷新界面（与 app_wifi 扫描完成后的做法一致） */
    if (lvgl_port_lock(0)) {
        lv_async_call(format_done_async, NULL);
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void format_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    if (btn != FW_DIALOG_BTN_OK) return;
    if (s_formatting) {
        fw_ui_toast("正在格式化", 1500);
        return;
    }

    s_fmt_type = (periph_storage_type_t)((intptr_t)user - 1);
    s_formatting = true;

    /* SPIFFS / FAT 格式化会长时间不让出 CPU，先停掉看门狗的超时重启，完成后恢复 */
    svc_watchdog_disarm();
    if (xTaskCreate(format_task, "app_fmt", FORMAT_STACK,
                    (void *)(intptr_t)s_fmt_type, 4, NULL) != pdPASS) {
        s_formatting = false;
        svc_watchdog_arm();
        fw_ui_toast("格式化失败", 2000);
    }
}

static void format_tf_cb(lv_event_t *e)
{
    (void)e;

    /* 未挂载（没插卡 / 已被拔出）时不弹确认框，直接提示 */
    periph_storage_info_t info;
    if (svc_storage_get_info(PERIPH_STORAGE_TF_CARD, &info) != ESP_OK) {
        fw_ui_toast("TF 卡未挂载", 2000);
        return;
    }
    fw_ui_dialog(NULL, "格式化 TF 卡", "将清空 TF 卡全部数据，且无法恢复，确定继续？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, format_confirm_cb,
                 (void *)(intptr_t)(PERIPH_STORAGE_TF_CARD + 1));
}

static void format_internal_cb(lv_event_t *e)
{
    (void)e;

    fw_ui_dialog(NULL, "格式化内置存储", "将清空内置存储全部数据，且无法恢复，确定继续？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, format_confirm_cb,
                 (void *)(intptr_t)(PERIPH_STORAGE_INTERNAL_FLASH + 1));
}

/* ------------------------------ 生命周期 ------------------------------ */

static lv_obj_t *bar_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 64, 36);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_center(label);
    return btn;
}

/* 危险操作入口：宽度更大，两个并排一行（保留文件列表的可用高度） */
static lv_obj_t *action_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 144, 36);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_center(label);
    return btn;
}

static void *file_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_rows == NULL) {
        s_rows = malloc((size_t)FILE_ROW_MAX * FILE_ROW_LEN);
    }
    if (s_row_dir == NULL) {
        s_row_dir = malloc(sizeof(bool) * FILE_ROW_MAX);
    }
    if (s_rows == NULL || s_row_dir == NULL) {
        fw_ui_toast("内存不足", 2000);
    }

    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    bar_btn(bar, "TF 卡", tf_cb);
    bar_btn(bar, "内置", internal_cb);
    bar_btn(bar, LV_SYMBOL_UP, up_cb);

    s_path = lv_label_create(bar);
    lv_obj_set_style_text_font(s_path, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_path, fw_theme_color_text_secondary(), 0);
    lv_label_set_long_mode(s_path, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_path, 96);

    /* 格式化入口（危险操作，点击后二次确认；见 STO-005） */
    lv_obj_t *bar2 = lv_obj_create(body);
    lv_obj_set_size(bar2, lv_pct(100), 36);
    lv_obj_set_scrollable(bar2, false);
    lv_obj_set_style_bg_opa(bar2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar2, 0, 0);
    lv_obj_set_style_pad_all(bar2, 0, 0);
    lv_obj_set_style_pad_column(bar2, 8, 0);
    lv_obj_set_flex_flow(bar2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    action_btn(bar2, "格式化 TF 卡", format_tf_cb);
    action_btn(bar2, "格式化内置存储", format_internal_cb);

    s_list = fw_ui_list(body, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    lvgl_port_unlock();

    scan();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void file_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_list = NULL;
    s_path = NULL;
    lvgl_port_unlock();

    free(s_rows);
    s_rows = NULL;
    free(s_row_dir);
    s_row_dir = NULL;
    s_row_count = 0;
}

/* 返回键：优先回上一级目录，已在根目录才退出 App */
static bool file_on_back(void *ctx)
{
    (void)ctx;

    if (!is_root_dir()) {
        go_up();
        return true;
    }
    return false;
}

const fw_app_desc_t app_file_desc = {
    .name = "File",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_DIRECTORY,
    .on_create = file_on_create,
    .on_destroy = file_on_destroy,
    .on_back = file_on_back,
};
