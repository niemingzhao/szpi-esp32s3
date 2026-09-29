/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - 锁屏实现
 *
 * 浮层挂在 lv_layer_top()，尺寸为整屏（含状态栏区域）：锁屏时状态栏按钮不可用。
 * fw_init 在控制中心 / 通知中心之后初始化它，保证它是最上层浮层。
 */

#include "fw_lockscreen.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.lockscreen";

static lv_obj_t *s_scrim = NULL;
static lv_obj_t *s_time = NULL;
static lv_timer_t *s_timer = NULL;
static bool s_locked = false;

static void refresh_time(void)
{
    if (s_time == NULL) return;

    char buf[8];
    if (!svc_time_is_synced() ||
        svc_time_format(svc_time_now(), "%H:%M", buf, sizeof(buf)) != ESP_OK) {
        lv_label_set_text(s_time, "--:--");
        return;
    }
    lv_label_set_text(s_time, buf);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_time();
}

/* 长按锁屏界面也能解锁（上滑是主路径，长按便于排查触摸手势） */
static void unlock_cb(lv_event_t *e)
{
    (void)e;
    fw_lockscreen_unlock();
}

static void build(void)
{
    lv_coord_t w = lv_disp_get_hor_res(NULL);
    lv_coord_t h = lv_disp_get_ver_res(NULL);

    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scrim, w, h);
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);   /* 吃掉触摸，不穿透到下面 */
    lv_obj_set_style_bg_color(s_scrim, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);
    lv_obj_add_event_cb(s_scrim, unlock_cb, LV_EVENT_LONG_PRESSED, NULL);

    s_time = lv_label_create(s_scrim);
    lv_obj_set_style_text_font(s_time, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_time, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -34);

    lv_obj_t *hint = lv_label_create(s_scrim);
    lv_label_set_text(hint, "已锁定  上滑解锁");
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 26);

    refresh_time();
}

esp_err_t fw_lockscreen_init(void)
{
    lvgl_port_lock(0);
    build();
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    s_locked = false;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t fw_lockscreen_rebuild(void)
{
    bool was_locked = s_locked;

    lvgl_port_lock(0);

    if (s_scrim != NULL) {
        lv_obj_del(s_scrim);
        s_scrim = NULL;
        s_time = NULL;
    }

    build();
    if (was_locked) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_lockscreen_lock(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    refresh_time();
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_locked = true;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "locked");
    return ESP_OK;
}

esp_err_t fw_lockscreen_unlock(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_locked) return ESP_OK;

    lvgl_port_lock(0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_locked = false;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "unlocked");
    return ESP_OK;
}

bool fw_lockscreen_is_locked(void)
{
    return s_locked;
}
