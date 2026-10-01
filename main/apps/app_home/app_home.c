/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Home（桌面）
 *
 * 一屏 4×2 = 8 个应用图标（时钟在左上角第一个），超出可左右滑动翻页（lv_tileview）。
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
#define HOME_ITEM_W     70
#define HOME_ITEM_H     86
#define HOME_APP_MAX    24

static lv_obj_t *s_root = NULL;

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

        lv_obj_t *icon = lv_label_create(cell);
        lv_label_set_text(icon, app->symbol ? app->symbol : fw_asset_symbol_for(app->name));
        lv_obj_set_style_text_font(icon, fw_asset_font_24(), 0);
        lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);

        lv_obj_t *label = lv_label_create(cell);
        lv_label_set_text(label, app->name);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, HOME_ITEM_W - 8);
        lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(label, fw_theme_color_text_secondary(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);
    } else {
        cell = lv_obj_create(grid);
        lv_obj_set_clickable(cell, false);
        /* 空槽贴近页面底色，只留下网格线，避免在浅色主题下看起来也像卡片 */
        lv_obj_set_style_bg_color(cell, fw_theme_color_bg_primary(), 0);
    }

    lv_obj_set_size(cell, HOME_ITEM_W, HOME_ITEM_H);
    lv_obj_set_scrollable(cell, false);
    lv_obj_set_style_radius(cell, 12, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, fw_theme_color_divider(), 0);
    lv_obj_set_style_shadow_width(cell, 0, 0);
}

static void *home_on_create(void)
{
    lvgl_port_lock(0);

    s_root = lv_obj_create(NULL);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_style_bg_color(s_root, fw_theme_color_bg_primary(), 0);

    /* 收集除 Home 自身以外的 App（注册顺序即网格顺序，Clock 在左上角第一个） */
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
        lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 6);

        for (size_t k = 0; k < HOME_PER_PAGE; k++) {
            size_t idx = p * HOME_PER_PAGE + k;
            add_cell(grid, (idx < cnt) ? items[idx] : NULL);
        }
    }

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
    lvgl_port_unlock();
}

const fw_app_desc_t app_home_desc = {
    .name = "Home",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_HOME,
    .on_create = home_on_create,
    .on_pause = home_on_pause,
    .on_resume = home_on_resume,
    .on_destroy = home_on_destroy,
};
