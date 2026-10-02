/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Sound（APP-SOUND 声音）
 *
 * 音量滑块 + 静音开关 + 试听提示音。音量与静音都由 svc_audio 持久化
 * （服务侧对写 NVS 做了消抖），重启后音量与静音状态都保持。
 *
 * 静音只静音输出，不把音量改成 0：音量值留着，取消静音即恢复原音量，
 * 也不会因为"静音一次"就丢掉用户设好的音量。
 */

#include "app_sound.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "app.sound";

#define TEST_TONE_HZ       880
#define TEST_TONE_MS       300

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_vol_slider = NULL;
static lv_obj_t *s_mute_row = NULL;
static bool s_muted = false;

static void volume_show(uint8_t vol)
{
    if (s_vol_slider == NULL) return;

    lv_slider_set_value(s_vol_slider, vol, LV_ANIM_OFF);

    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)vol);
    fw_ui_slider_row_value(s_vol_slider, buf);
}

/* 行标签是「静音」，数值就是静音本身的状态：开 = 已静音 */
static void mute_show(void)
{
    if (s_mute_row != NULL) {
        fw_ui_row_btn_value(s_mute_row, s_muted ? "开" : "关");
    }
}

static void volume_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const uint8_t vol = (uint8_t)lv_slider_get_value(slider);

    /* 拖动音量即视为取消静音 */
    if (vol > 0 && s_muted) {
        s_muted = false;
        svc_audio_set_mute(false);
        mute_show();
    }
    svc_audio_set_volume(vol);

    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)vol);
    fw_ui_slider_row_value(slider, buf);
}

static void mute_cb(lv_event_t *e)
{
    (void)e;

    s_muted = !s_muted;
    svc_audio_set_mute(s_muted);
    mute_show();
}

static void test_cb(lv_event_t *e)
{
    (void)e;

    if (s_muted || svc_audio_get_volume() == 0) {
        fw_ui_toast("当前静音", 1500);
        return;
    }
    svc_audio_play_tone_async(TEST_TONE_HZ, TEST_TONE_MS);
}

static void *sound_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 与服务的真实状态同步（App 重建 / 别处改过静音都不会显示错） */
    s_muted = svc_audio_get_mute();

    s_vol_slider = fw_ui_slider_row(body, "音量", 0, 100, svc_audio_get_volume(), volume_cb, NULL);
    volume_show(svc_audio_get_volume());

    s_mute_row = fw_ui_row_btn(body, LV_SYMBOL_MUTE, "静音", mute_cb, NULL);
    mute_show();

    fw_ui_row_btn(body, LV_SYMBOL_AUDIO, "试听提示音", test_cb, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void sound_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_vol_slider = NULL;
    s_mute_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_sound_desc = {
    .name = "Sound",
    .title = "声音",
    .icon_64 = &icon_home_sound,
    .symbol = LV_SYMBOL_AUDIO,
    .on_create = sound_on_create,
    .on_destroy = sound_on_destroy,
};
