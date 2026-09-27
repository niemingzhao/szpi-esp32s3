/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Boot Animation 实现
 *
 * 开机画面：全屏黑底 + 立创官方 120x120 Logo（静态展示），
 * 同时播放一声 1 kHz / 300 ms 提示音（先解除静音并等功放稳定）；
 * 用户在「声音」里设了静音则跳过提示音，且不改动静音状态。
 * 展示结束后由 main 切到桌面。
 */

#include "fw_boot_animation.h"
#include "fw_common.h"
#include "fw_logo.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "fw.boot_anim";

#define BOOT_BEEP_MS      300     /* 开机提示音时长 */
#define BOOT_BEEP_HZ      1000
#define BOOT_AMP_MS       150     /* 等功放 / codec 解除静音稳定，避免把提示音开头吃掉 */
#define BOOT_HOLD_MS      1500    /* 提示音结束后的停留时间（画面总时长 = 150+300+100+1500 = 2050 ms） */

esp_err_t fw_boot_animation(void)
{
    lvgl_port_lock(0);

    lv_obj_t *prev = lv_screen_active();

    /* 开机画面要占满整屏：先藏起状态栏等全局浮层 */
    lv_obj_set_hidden(lv_layer_top(), true);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *logo = lv_image_create(scr);
    lv_image_set_src(logo, &image_lckfb_logo);
    lv_obj_center(logo);

    lv_screen_load(scr);

    lvgl_port_unlock();

    /* 开机提示音：先打开功放并等它稳定，再放一声长滴，
     * 否则静音解除的斜坡会把提示音开头吃掉（表现为"听不到"）。
     * 用户设了静音就不响，也不去动"静音"状态 —— 否则每次开机都会把静音清掉，
     * 表现就是"静音记不住"（曾经如此：这里无条件调了 svc_audio_set_mute(false)）。
     * 等待时间不受静音影响，保证开机画面时长稳定。 */
    const bool muted = svc_audio_get_mute();
    if (!muted) {
        svc_audio_set_mute(false);
    }
    vTaskDelay(pdMS_TO_TICKS(BOOT_AMP_MS));

    if (!muted) {
        svc_audio_play_tone_async(BOOT_BEEP_HZ, BOOT_BEEP_MS);
    }
    vTaskDelay(pdMS_TO_TICKS(BOOT_BEEP_MS + 100));

    vTaskDelay(pdMS_TO_TICKS(BOOT_HOLD_MS));

    lvgl_port_lock(0);
    lv_obj_set_hidden(lv_layer_top(), false);
    if (prev != NULL) lv_screen_load(prev);
    /* 绝不当场删除活动屏 */
    if (lv_screen_active() != scr) lv_obj_delete(scr);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "done");
    return ESP_OK;
}
