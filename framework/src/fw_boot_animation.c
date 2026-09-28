/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Boot Animation 实现
 *
 * 动画在 LVGL 任务里跑，本函数只负责搭台与等待，等待时不能持锁，
 * 否则 LVGL 任务无法刷新、动画不会推进。
 */

#include "fw_boot_animation.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "fw.boot_anim";

static SemaphoreHandle_t s_done = NULL;

static void anim_zoom_cb(void *obj, int32_t v)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)obj, v, 0);
}

static void anim_angle_cb(void *obj, int32_t v)
{
    lv_obj_set_style_transform_angle((lv_obj_t *)obj, v, 0);
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* 淡出用独立回调：lv_anim_start() 会删掉同 var + 同 exec_cb 的旧动画，
 * 若淡入淡出共用回调，淡入会被尚未开始的淡出顶掉。 */
static void anim_opa_out_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_done != NULL) xSemaphoreGive(s_done);
}

esp_err_t fw_boot_animation(void)
{
    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
        if (s_done == NULL) return ESP_ERR_NO_MEM;
    }

    lvgl_port_lock(0);

    lv_obj_t *prev = lv_scr_act();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *logo = lv_label_create(scr);
    lv_label_set_text(logo, "SZPI-OS");
    lv_obj_set_style_text_font(logo, fw_asset_font_32(), 0);
    lv_obj_set_style_text_color(logo, fw_theme_color_accent(), 0);
    lv_obj_center(logo);
    lv_obj_set_style_opa(logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_zoom(logo, 64, 0);

    /* 旋转 / 缩放围绕 Logo 中心 */
    lv_obj_update_layout(logo);
    lv_obj_set_style_transform_pivot_x(logo, lv_obj_get_width(logo) / 2, 0);
    lv_obj_set_style_transform_pivot_y(logo, lv_obj_get_height(logo) / 2, 0);

    lv_scr_load(scr);

    lv_anim_t a;

    /* 1) 缩放淡入 300 ms */
    lv_anim_init(&a);
    lv_anim_set_var(&a, logo);
    lv_anim_set_values(&a, 64, 256);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, anim_zoom_cb);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, logo);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_start(&a);

    /* 2) 旋转一圈 500 ms，延迟 300 ms */
    lv_anim_init(&a);
    lv_anim_set_var(&a, logo);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, 500);
    lv_anim_set_delay(&a, 300);
    lv_anim_set_exec_cb(&a, anim_angle_cb);
    lv_anim_start(&a);

    /* 3) 淡出 300 ms，延迟 800 ms，结束时唤醒等待方 */
    lv_anim_init(&a);
    lv_anim_set_var(&a, logo);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 300);
    lv_anim_set_delay(&a, 800);
    lv_anim_set_exec_cb(&a, anim_opa_out_cb);
    lv_anim_set_ready_cb(&a, anim_done_cb);
    lv_anim_start(&a);

    lvgl_port_unlock();

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(2500)) != pdTRUE) {
        ESP_LOGW(TAG, "boot animation timeout");
    }

    lvgl_port_lock(0);
    if (prev != NULL) lv_scr_load(prev);
    lv_obj_del(scr);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "done");
    return ESP_OK;
}
