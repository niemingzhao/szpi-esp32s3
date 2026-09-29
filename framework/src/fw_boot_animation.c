/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Boot Animation 实现
 *
 * 开机画面：全屏黑底 + 立创官方 120x120 Logo（静态展示），
 * 同时播放一声 1 kHz / 300 ms 提示音（先解除静音并等功放稳定），
 * 展示结束后由 main 切到桌面。
 */

#include "fw_boot_animation.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_IMG_DECLARE(image_lckfb_logo);

static const char *TAG = "fw.boot_anim";

#define BOOT_BEEP_MS      300     /* 开机提示音时长 */
#define BOOT_BEEP_HZ      1000
#define BOOT_AMP_MS       150     /* 等功放 / codec 解除静音稳定，避免把提示音开头吃掉 */
#define BOOT_HOLD_MS      800     /* 提示音结束后的停留时间 */

esp_err_t fw_boot_animation(void)
{
    lvgl_port_lock(0);

    lv_obj_t *prev = lv_scr_act();

    /* 开机画面要占满整屏：先藏起状态栏等全局浮层 */
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *logo = lv_img_create(scr);
    lv_img_set_src(logo, &image_lckfb_logo);
    lv_obj_center(logo);

    lv_scr_load(scr);

    lvgl_port_unlock();

    /* 开机提示音：先打开功放并等它稳定，再放一声长滴，
     * 否则静音解除的斜坡会把提示音开头吃掉（表现为"听不到"） */
    svc_audio_set_mute(false);
    vTaskDelay(pdMS_TO_TICKS(BOOT_AMP_MS));

    svc_audio_play_tone_async(BOOT_BEEP_HZ, BOOT_BEEP_MS);
    vTaskDelay(pdMS_TO_TICKS(BOOT_BEEP_MS + 100));

    vTaskDelay(pdMS_TO_TICKS(BOOT_HOLD_MS));

    lvgl_port_lock(0);
    lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_HIDDEN);
    if (prev != NULL) lv_scr_load(prev);
    /* 绝不当场删除活动屏（见 AGENTS 4.13） */
    if (lv_scr_act() != scr) lv_obj_del(scr);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "done");
    return ESP_OK;
}
