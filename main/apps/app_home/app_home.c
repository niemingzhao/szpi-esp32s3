/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Home（桌面）
 *
 * 一屏 4×2 = 8 个应用格子，超出可左右滑动翻页（lv_tileview）。
 * 每个格子：上方彩色图标（App 描述符的 icon），下方中文标题（描述符的 title），
 * 标题居中、只占一行，放不下（超过 4 个汉字）由 LVGL 的 dots 模式换成 "..."。
 * 标题顺序 = App 注册顺序。
 * 顶部状态栏由 fw_statusbar 提供，不属于桌面内容。
 */

#include "app_home.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app.home";

#define HOME_COLS       4
#define HOME_ROWS       2
#define HOME_PER_PAGE   (HOME_COLS * HOME_ROWS)
#define HOME_ITEM_W     73
#define HOME_ITEM_H     76
#define HOME_GAP        5      /* 列间距与网格内边距；行间距沿用 fw_ui_grid 的 10 */
#define HOME_APP_MAX    24
#define HOME_PAGE_MAX   6

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_tiles[HOME_PAGE_MAX] = { 0 };
static lv_obj_t *s_dots[HOME_PAGE_MAX] = { 0 };
static size_t s_page_cnt = 0;
static size_t s_page_cur = 0;

/* 页码指示点：当前页强调色，其余用分隔线色 */
static void page_refresh(void)
{
    for (size_t i = 0; i < s_page_cnt; i++) {
        if (s_dots[i] == NULL) continue;
        lv_obj_set_style_bg_color(s_dots[i],
                                  (i == s_page_cur) ? fw_theme_color_accent()
                                                    : fw_theme_color_divider(), 0);
    }
}

static void tile_changed_cb(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    if (tv == NULL) return;

    lv_obj_t *act = lv_tileview_get_tile_active(tv);
    for (size_t i = 0; i < s_page_cnt; i++) {
        if (s_tiles[i] == act) {
            s_page_cur = i;
            break;
        }
    }
    page_refresh();
}

static void launch_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (name != NULL) {
        fw_app_mgr_launch(name);
    }
}

/* 一个网格单元：有 App 时可点卡片，无 App 时空槽（保持网格完整，不显得空洞） */
static void add_cell(lv_obj_t *grid, const fw_app_desc_t *app)
{
    lv_obj_t *cell;

    if (app != NULL) {
        cell = lv_button_create(grid);
        lv_obj_add_event_cb(cell, launch_cb, LV_EVENT_SHORT_CLICKED, (void *)app->name);
        lv_obj_set_style_bg_color(cell, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_border_color(cell, fw_theme_color_border(), 0);

        if (app->icon != NULL) {
            lv_obj_t *icon = lv_image_create(cell);
            lv_image_set_src(icon, app->icon);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 7);
        }

        /* 标题只占一行：宽度够放 4 个汉字 + "..."，放不下由 dots 模式截断 */
        lv_obj_t *label = lv_label_create(cell);
        lv_obj_set_width(label, HOME_ITEM_W - 2);
        lv_obj_set_height(label, lv_font_get_line_height(fw_asset_font_cn()));
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(label, app->title != NULL ? app->title : app->name);
        lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
    } else {
        cell = lv_obj_create(grid);
        lv_obj_set_clickable(cell, false);
        /* 空槽贴近页面底色，只留下分隔线色的网格线，避免在浅色主题下看起来也像卡片 */
        lv_obj_set_style_bg_color(cell, fw_theme_color_bg_primary(), 0);
        lv_obj_set_style_border_color(cell, fw_theme_color_divider(), 0);
    }

    lv_obj_set_size(cell, HOME_ITEM_W, HOME_ITEM_H);
    lv_obj_set_scrollable(cell, false);
    lv_obj_set_style_radius(cell, 12, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_shadow_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
}

static void *home_on_create(void)
{
    lvgl_port_lock(0);

    s_root = lv_obj_create(NULL);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_style_bg_color(s_root, fw_theme_color_bg_primary(), 0);

    /* 收集除 Home 自身以外的 App：注册顺序即网格顺序，脚本管理在左上角第一个 */
    const fw_app_desc_t *apps[HOME_APP_MAX];
    size_t n = fw_app_mgr_list(apps, HOME_APP_MAX);
    const fw_app_desc_t *items[HOME_APP_MAX];
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(apps[i]->name, FW_APP_HOME_NAME) == 0) continue;
        items[cnt++] = apps[i];
    }

    size_t pages = (cnt + HOME_PER_PAGE - 1) / HOME_PER_PAGE;
    if (pages == 0) pages = 1;

    lv_obj_t *tv = lv_tileview_create(s_root);
    lv_obj_set_size(tv, lv_display_get_horizontal_resolution(lv_display_get_default()), lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H);
    lv_obj_align(tv, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    for (size_t p = 0; p < pages; p++) {
        lv_obj_t *tile = lv_tileview_add_tile(tv, p, 0, LV_DIR_HOR);
        lv_obj_set_scrollable(tile, false);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);

        lv_obj_t *grid = fw_ui_grid(tile, HOME_COLS, HOME_ITEM_W, HOME_ITEM_H);
        /* fw_ui_grid 的默认间距（列 8 / 内边距 6）偏大，桌面收紧列间距与内边距并把宽度
         * 算准；行间距保持它的默认值 10（两行之间留松一点） */
        lv_obj_set_style_pad_all(grid, HOME_GAP, 0);
        lv_obj_set_style_pad_column(grid, HOME_GAP, 0);
        lv_obj_set_width(grid, HOME_COLS * HOME_ITEM_W + (HOME_COLS - 1) * HOME_GAP + 2 * HOME_GAP);
        lv_obj_align(grid, LV_ALIGN_CENTER, 0, 0);

        for (size_t k = 0; k < HOME_PER_PAGE; k++) {
            size_t idx = p * HOME_PER_PAGE + k;
            add_cell(grid, (idx < cnt) ? items[idx] : NULL);
        }

        if (p < HOME_PAGE_MAX) s_tiles[p] = tile;
    }

    /* 多页时在底部放页码点：提示可以左右滑动，也标出当前页 */
    s_page_cnt = (pages < HOME_PAGE_MAX) ? pages : HOME_PAGE_MAX;
    s_page_cur = 0;

    if (s_page_cnt > 1) {
        lv_obj_t *dots = lv_obj_create(s_root);
        lv_obj_set_size(dots, LV_SIZE_CONTENT, 8);
        lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(dots, 0, 0);
        lv_obj_set_style_pad_all(dots, 0, 0);
        lv_obj_set_style_pad_column(dots, 6, 0);
        lv_obj_set_scrollable(dots, false);
        lv_obj_set_clickable(dots, false);
        lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, -6);

        for (size_t i = 0; i < s_page_cnt; i++) {
            lv_obj_t *dot = lv_obj_create(dots);
            lv_obj_set_size(dot, 6, 6);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, fw_theme_color_divider(), 0);
            lv_obj_set_scrollable(dot, false);
            lv_obj_set_clickable(dot, false);
            s_dots[i] = dot;
        }
        page_refresh();
    }

    lv_obj_add_event_cb(tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (%u apps, %u page(s))", (unsigned)cnt, (unsigned)pages);
    return s_root;
}

static void home_on_pause(void *ctx)
{
    (void)ctx;
}

static void home_on_resume(void *ctx)
{
    (void)ctx;
}

static void home_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    for (size_t i = 0; i < HOME_PAGE_MAX; i++) {
        s_tiles[i] = NULL;
        s_dots[i] = NULL;
    }
    s_page_cnt = 0;
    s_page_cur = 0;
    lvgl_port_unlock();
}

const fw_app_desc_t app_home_desc = {
    .name = "Home",
    .title = "桌面",
    .icon = NULL,
    .on_create = home_on_create,
    .on_pause = home_on_pause,
    .on_resume = home_on_resume,
    .on_destroy = home_on_destroy,
};
