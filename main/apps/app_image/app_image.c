/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Image（APP-IMAGE 图库）
 *
 * 扫描 TF 卡 / 内置存储根目录的 PNG / JPEG / GIF / BMP，网格点选后全屏查看。
 * JPEG 由 LVGL 的 TJPGD 解码器支持（sdkconfig.defaults 已开 LV_USE_TJPGD）。
 * PRD 写的是"左右滑动切换"，但 UI 规范明确不使用滑动手势，所以这里用上一张 /
 * 下一张两个按钮代替（同规格的做法见 AGENTS）。
 */

#include "app_image.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.image";

#define IMG_MAX       12
#define IMG_NAME_MAX  64
#define IMG_PATH_MAX  300

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_grid = NULL;
static lv_obj_t *s_viewer = NULL;      /* 全屏查看层（盖住内容区） */
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_view_name = NULL;

static char s_base[16] = "/sdcard";
/* 图片文件名列表放堆上（IMG_MAX × IMG_NAME_MAX = 768 B），不在 App 里放大静态数组（见 AGENTS 4.20） */
static char (*s_names)[IMG_NAME_MAX] = NULL;
static size_t s_count = 0;
static int s_index = -1;

/* ------------------------------- 文件 ------------------------------- */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n <= e) return false;

    const char *p = name + (n - e);
    for (size_t i = 0; i < e; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[i]) return false;
    }
    return true;
}

static bool is_image(const char *name)
{
    return has_ext(name, ".png") || has_ext(name, ".jpg") || has_ext(name, ".jpeg") ||
           has_ext(name, ".gif") || has_ext(name, ".bmp");
}

static void build_path(size_t idx, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", s_base, s_names[idx]);
}

/* ------------------------------ 查看层 ------------------------------ */

static void show_current(void)
{
    if (s_img == NULL || s_view_name == NULL) return;
    if (s_index < 0 || (size_t)s_index >= s_count) return;

    char path[IMG_PATH_MAX];
    char fs_path[IMG_PATH_MAX + 4];
    build_path((size_t)s_index, path, sizeof(path));

    if (fw_asset_fs_path(path, fs_path, sizeof(fs_path)) == ESP_OK) {
        lv_image_set_src(s_img, fs_path);
    }

    char title[IMG_NAME_MAX + 16];
    snprintf(title, sizeof(title), "%.40s (%d/%u)", s_names[s_index], s_index + 1, (unsigned)s_count);
    lv_label_set_text(s_view_name, title);
}

static void close_viewer(void)
{
    if (s_viewer == NULL) return;

    lvgl_port_lock(0);
    lv_obj_delete(s_viewer);
    s_viewer = NULL;
    s_img = NULL;
    s_view_name = NULL;
    lvgl_port_unlock();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;
    s_index = (s_index <= 0) ? (int)s_count - 1 : s_index - 1;
    show_current();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;
    s_index = ((size_t)(s_index + 1) >= s_count) ? 0 : s_index + 1;
    show_current();
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    close_viewer();
}

static void open_index(int idx)
{
    if (idx < 0 || (size_t)idx >= s_count) return;

    s_index = idx;

    lvgl_port_lock(0);

    lv_obj_t *body = lv_obj_get_parent(s_grid);
    s_viewer = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_viewer, lv_pct(100), lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H);
    lv_obj_set_pos(s_viewer, 0, FW_STATUSBAR_H);
    lv_obj_set_scrollable(s_viewer, false);
    lv_obj_set_style_bg_color(s_viewer, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(s_viewer, 6, 0);
    lv_obj_set_style_border_width(s_viewer, 0, 0);

    s_img = lv_image_create(s_viewer);
    lv_obj_center(s_img);

    /* 顶部文件名 + 底部翻页按钮 */
    lv_obj_t *bar = lv_obj_create(s_viewer);
    lv_obj_set_size(bar, lv_pct(100), 36);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *prev = lv_button_create(bar);
    lv_obj_set_size(prev, 56, 32);
    lv_obj_set_style_bg_color(prev, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(prev, 8, 0);
    lv_obj_set_style_shadow_width(prev, 0, 0);
    lv_obj_set_style_border_width(prev, 1, 0);
    lv_obj_set_style_border_color(prev, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(prev, prev_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *prev_l = lv_label_create(prev);
    lv_label_set_text(prev_l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prev_l, fw_theme_color_accent(), 0);
    lv_obj_center(prev_l);

    s_view_name = lv_label_create(bar);
    lv_obj_set_style_text_font(s_view_name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_view_name, fw_theme_color_text_secondary(), 0);
    lv_label_set_long_mode(s_view_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_view_name, 150);

    lv_obj_t *close = lv_button_create(bar);
    lv_obj_set_size(close, 56, 32);
    lv_obj_set_style_bg_color(close, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_border_color(close, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *close_l = lv_label_create(close);
    lv_label_set_text(close_l, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_l, fw_theme_color_text_primary(), 0);
    lv_obj_center(close_l);

    lv_obj_t *next = lv_button_create(bar);
    lv_obj_set_size(next, 56, 32);
    lv_obj_set_style_bg_color(next, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(next, 8, 0);
    lv_obj_set_style_shadow_width(next, 0, 0);
    lv_obj_set_style_border_width(next, 1, 0);
    lv_obj_set_style_border_color(next, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(next, next_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *next_l = lv_label_create(next);
    lv_label_set_text(next_l, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(next_l, fw_theme_color_accent(), 0);
    lv_obj_center(next_l);

    (void)body;
    show_current();

    lvgl_port_unlock();
}

/* ------------------------------ 网格 ------------------------------ */

static void tile_cb(lv_event_t *e)
{
    open_index((int)(intptr_t)lv_event_get_user_data(e));
}

static void scan(void)
{
    s_count = 0;

    if (s_names == NULL) return;    /* 内存不足：on_create 已提示 */

    static const char *bases[] = { "/sdcard", "/sdcard/DCIM", "/internal" };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]) && s_count == 0; b++) {
        svc_storage_iter_t it = NULL;
        if (svc_storage_iter_start(bases[b], &it) != ESP_OK) continue;

        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL) {
            if (entry->is_dir || !is_image(entry->name)) continue;
            if (s_count >= IMG_MAX) break;

            strlcpy(s_names[s_count], entry->name, IMG_NAME_MAX);
            s_count++;
        }
        svc_storage_iter_end(it);
        if (s_count > 0) strlcpy(s_base, bases[b], sizeof(s_base));
    }

    if (s_grid != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_grid);
        for (size_t i = 0; i < s_count; i++) {
            lv_obj_t *tile = lv_button_create(s_grid);
            lv_obj_set_size(tile, 135, 96);
            lv_obj_set_scrollable(tile, false);
            lv_obj_set_style_bg_color(tile, fw_theme_color_bg_card(), 0);
            lv_obj_set_style_radius(tile, 10, 0);
            lv_obj_set_style_shadow_width(tile, 0, 0);
            lv_obj_set_style_border_width(tile, 1, 0);
            lv_obj_set_style_border_color(tile, fw_theme_color_border(), 0);
            lv_obj_set_style_pad_all(tile, 6, 0);
            lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *icon = lv_label_create(tile);
            lv_label_set_text(icon, LV_SYMBOL_IMAGE);
            lv_obj_set_style_text_font(icon, fw_asset_font_20(), 0);
            lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);

            lv_obj_t *name = lv_label_create(tile);
            lv_label_set_text(name, s_names[i]);
            lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name, lv_pct(100));
            lv_obj_set_style_text_font(name, fw_asset_font_cn(), 0);
            lv_obj_set_style_text_color(name, fw_theme_color_text_primary(), 0);
            lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "scan %s: %u image(s)", s_base, (unsigned)s_count);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *image_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_names == NULL) {
        s_names = malloc((size_t)IMG_MAX * IMG_NAME_MAX);
    }
    if (s_names == NULL) {
        fw_ui_toast("内存不足", 2000);
    }

    s_grid = fw_ui_grid(body, 2, 135, 96);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_flex_grow(s_grid, 1);

    scan();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void image_on_destroy(void *ctx)
{
    (void)ctx;

    close_viewer();

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_grid = NULL;
    lvgl_port_unlock();

    free(s_names);
    s_names = NULL;
    s_count = 0;
    s_index = -1;
}

/* 查看层打开时，返回键先关它（返回 true 表示 App 内已处理） */
static bool image_on_back(void *ctx)
{
    (void)ctx;

    if (s_viewer != NULL) {
        close_viewer();
        return true;
    }
    return false;
}

const fw_app_desc_t app_image_desc = {
    .name = "Image",
    .title = "图库",
    .icon_64 = &icon_home_image,
    .symbol = LV_SYMBOL_IMAGE,
    .on_create = image_on_create,
    .on_destroy = image_on_destroy,
    .on_back = image_on_back,
};
