/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Audio（骨架）
 *
 * 说明：Peripherals 音频后端（I2S + ES8311/ES7210）尚未实现，
 *       本服务先提供完整接口与音量/静音控制，播放/录音返回 ESP_ERR_NOT_SUPPORTED。
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"

static const char *TAG = "svc.audio";

static svc_audio_state_t s_state = SVC_AUDIO_STATE_IDLE;
static svc_audio_cb_t s_cb = NULL;
static void *s_user = NULL;

esp_err_t svc_audio_init(void)
{
    periph_audio_init();
    s_state = SVC_AUDIO_STATE_IDLE;
    ESP_LOGI(TAG, "initialized (skeleton)");
    return ESP_OK;
}

esp_err_t svc_audio_deinit(void)
{
    s_state = SVC_AUDIO_STATE_IDLE;
    return ESP_OK;
}

esp_err_t svc_audio_play(const svc_audio_source_t *src)
{
    if (src == NULL) return ESP_ERR_INVALID_ARG;
    ESP_LOGW(TAG, "play not implemented (no I2S backend)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_pause(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_resume(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds)
{
    (void)file_path;
    (void)max_seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_record_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_set_volume(uint8_t percent)
{
    return periph_audio_set_volume(percent);
}

uint8_t svc_audio_get_volume(void)
{
    return periph_audio_get_volume();
}

esp_err_t svc_audio_set_mute(bool mute)
{
    return periph_audio_set_mute(mute);
}

svc_audio_state_t svc_audio_get_state(void)
{
    return s_state;
}

esp_err_t svc_audio_register_callback(svc_audio_cb_t cb, void *user)
{
    s_cb = cb;
    s_user = user;
    return ESP_OK;
}

esp_err_t svc_audio_play_tone_async(uint16_t freq_hz, uint32_t ms)
{
    (void)freq_hz;
    (void)ms;
    return ESP_ERR_NOT_SUPPORTED;
}
