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
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "app.file";

#define FILE_ROW_MAX   16
#define FILE_ROW_LEN   64
#define FILE_PATH_MAX  128

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_path = NULL;

static char s_cur[FILE_PATH_MAX] = "/sdcard";   /* 当前目录 */
static char s_rows[FILE_ROW_MAX][FILE_ROW_LEN];
static bool s_row_dir[FILE_ROW_MAX];
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

/* ------------------------------ 生命周期 ------------------------------ */

static lv_obj_t *bar_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 64, 36);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
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

    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
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
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_list = NULL;
    s_path = NULL;
    lvgl_port_unlock();
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
