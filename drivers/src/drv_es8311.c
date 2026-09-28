/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - ES8311 音频 DAC（I2C0 @ 0x18）
 *
 * 使用 espressif/es8311 组件（基于旧版 driver/i2c.h，与 bsp_init 的 I2C0 一致）。
 * MCLK 由 I2S 主机提供，倍频固定 256。
 */

#include "drv_common.h"
#include "es8311.h"
#include "esp_log.h"

static const char *TAG = "drv.es8311";

#define DRV_ES8311_MCLK_MULTIPLE   256

static es8311_handle_t s_dev = NULL;
static uint32_t s_sample_rate = 0;

esp_err_t drv_es8311_init(uint32_t sample_rate)
{
    if (sample_rate == 0) return ESP_ERR_INVALID_ARG;

    if (s_dev == NULL) {
        s_dev = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
        if (s_dev == NULL) {
            ESP_LOGE(TAG, "es8311_create failed");
            return ESP_FAIL;
        }
    }

    es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = (int)(sample_rate * DRV_ES8311_MCLK_MULTIPLE),
        .sample_frequency = (int)sample_rate,
    };

    esp_err_t err = es8311_init(s_dev, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = es8311_microphone_config(s_dev, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311_microphone_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sample_rate = sample_rate;
    ESP_LOGI(TAG, "initialized (%u Hz)", (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t drv_es8311_set_sample_rate(uint32_t sample_rate)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (sample_rate == 0) return ESP_ERR_INVALID_ARG;
    if (sample_rate == s_sample_rate) return ESP_OK;

    esp_err_t err = es8311_sample_frequency_config(
        s_dev, (int)(sample_rate * DRV_ES8311_MCLK_MULTIPLE), (int)sample_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set sample rate %u failed: %s", (unsigned)sample_rate, esp_err_to_name(err));
        return err;
    }

    s_sample_rate = sample_rate;
    ESP_LOGI(TAG, "sample rate -> %u Hz", (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t drv_es8311_set_volume(uint8_t percent)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (percent > 100) percent = 100;

    int set = 0;
    esp_err_t err = es8311_voice_volume_set(s_dev, (int)percent, &set);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "volume set to %d%%", set);
    return ESP_OK;
}

esp_err_t drv_es8311_set_mute(bool mute)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    return es8311_voice_mute(s_dev, mute);
}

esp_err_t drv_es8311_deinit(void)
{
    if (s_dev != NULL) {
        es8311_delete(s_dev);
        s_dev = NULL;
    }
    s_sample_rate = 0;
    return ESP_OK;
}
