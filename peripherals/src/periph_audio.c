/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Audio (ES8311 DAC + ES7210 ADC)
 *
 * 说明：PA_EN 由 PCA9557 控制；ES8311 / ES7210 的 I2S 通路未接入
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "periph.audio";

static uint8_t s_volume = 80;
static bool s_muted = false;
static bool s_initialized = false;

static periph_audio_format_t s_fmt_play = {
    .sample_rate = PERIPH_AUDIO_SR_48K, .bit_width = 16, .channels = 2,
};
static periph_audio_format_t s_fmt_record = {
    .sample_rate = PERIPH_AUDIO_SR_16K, .bit_width = 16, .channels = 2,
};

esp_err_t periph_audio_init(void)
{
    if (s_initialized) return ESP_OK;
    drv_pca9557_set_pin(DRV_PCA9557_PA_EN, 0);  // PA 默认关闭
    s_initialized = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t periph_audio_set_format(periph_audio_dir_t dir, const periph_audio_format_t *fmt)
{
    if (fmt == NULL) return ESP_ERR_INVALID_ARG;

    if (dir == PERIPH_AUDIO_DIR_RECORD) {
        s_fmt_record = *fmt;
    } else {
        s_fmt_play = *fmt;
    }
    ESP_LOGI(TAG, "format set: dir=%d sr=%d bits=%d ch=%d",
             dir, fmt->sample_rate, fmt->bit_width, fmt->channels);
    return ESP_OK;
}

esp_err_t periph_audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;
    ESP_LOGI(TAG, "volume set to %d%%", percent);
    return ESP_OK;
}

uint8_t periph_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t periph_audio_set_mute(bool mute)
{
    s_muted = mute;
    drv_pca9557_set_pin(DRV_PCA9557_PA_EN, mute ? 0 : 1);
    ESP_LOGI(TAG, "mute: %s", mute ? "on" : "off");
    return ESP_OK;
}

esp_err_t periph_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void)data; (void)len; (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;  // 未接入 I2S
}

esp_err_t periph_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)data; (void)len; (void)timeout_ms;
    if (bytes_read) *bytes_read = 0;
    return ESP_ERR_NOT_SUPPORTED;  // 未接入 I2S
}
