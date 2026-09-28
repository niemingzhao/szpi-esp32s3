/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Home（桌面）
 *
 * 桌面只负责填充分屏内容区；顶部状态栏与底部虚拟按键栏由
 * fw_statusbar / fw_input 以全局浮层形式创建，不随屏幕切换消失。
 */

#include "app_home.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "app.home";

#define HOME_GRID_MAX   24

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_grid = NULL;

static void launch_cb(lv_event_t *e)
{
    const char *name = (const char *)lv_event_get_user_data(e);
    if (name != NULL) {
        fw_app_mgr_launch(name);
    }
}

static void build_grid(void)
{
    const fw_app_desc_t *apps[HOME_GRID_MAX];
    size_t n = fw_app_mgr_list(apps, HOME_GRID_MAX);

    for (size_t i = 0; i < n; i++) {
        if (strcmp(apps[i]->name, FW_APP_HOME_NAME) == 0) continue;

        lv_obj_t *btn = lv_btn_create(s_grid);
        lv_obj_set_size(btn, 78, 82);
        lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, launch_cb, LV_EVENT_CLICKED, (void *)apps[i]->name);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, apps[i]->symbol ? apps[i]->symbol
                                                : fw_asset_symbol_for(apps[i]->name));
        lv_obj_set_style_text_font(icon, fw_asset_font_24(), 0);
        lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, apps[i]->name);
        lv_obj_set_style_text_font(label, fw_asset_font_20(), 0);
        lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -6);
    }
}

static void *home_on_create(void)
{
    lvgl_port_lock(0);

    s_root = lv_obj_create(NULL);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_root, fw_theme_color_bg_primary(), 0);

    s_grid = fw_ui_grid(s_root, 3, 78, 82);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 30);

    build_grid();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
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
        lv_obj_del(s_root);
        s_root = NULL;
        s_grid = NULL;
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
