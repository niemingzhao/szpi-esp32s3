/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Sound（APP-SOUND 声音）
 *
 * 音量滑块 + 静音开关 + 试听提示音。音量由 svc_audio 持久化。
 */

#include "app_sound.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "app.sound";

#define TEST_TONE_HZ     880
#define TEST_TONE_MS     300
#define VOLUME_ON_MUTE   70      /* 取消静音时恢复的音量 */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_mute_row = NULL;
static bool s_muted = false;

static void mute_value(void)
{
    if (s_mute_row != NULL) {
        fw_ui_row_btn_value(s_mute_row, s_muted ? "关" : "开");
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
        mute_value();
    }
    svc_audio_set_volume(vol);
}

static void mute_cb(lv_event_t *e)
{
    (void)e;

    s_muted = !s_muted;
    svc_audio_set_mute(s_muted);
    if (!s_muted) {
        svc_audio_set_volume(VOLUME_ON_MUTE);
    }
    mute_value();
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

    fw_ui_slider_row(body, "音量", 0, 100, svc_audio_get_volume(), volume_cb, NULL);

    s_mute_row = fw_ui_row_btn(body, LV_SYMBOL_MUTE, "静音", mute_cb, NULL);
    mute_value();

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
    s_mute_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_sound_desc = {
    .name = "Sound",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_AUDIO,
    .on_create = sound_on_create,
    .on_destroy = sound_on_destroy,
};
