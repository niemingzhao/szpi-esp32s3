/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Audio (ES8311 DAC + I2S)
 *
 * - 控制：ES8311 走 I2C0（drv_es8311）
 * - 数据：I2S0 发送通道，MCLK GPIO38 / BCLK GPIO14 / WS GPIO13 / DOUT GPIO45
 * - 功放：PA_EN 由 PCA9557.BIT1 控制（静音时关闭）
 * - 录音（ES7210 + I2S RX）尚未接入
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "periph.audio";

#define PERIPH_AUDIO_I2S_PORT    I2S_NUM_0
#define PERIPH_AUDIO_MCLK_GPIO   GPIO_NUM_38
#define PERIPH_AUDIO_BCLK_GPIO   GPIO_NUM_14
#define PERIPH_AUDIO_WS_GPIO     GPIO_NUM_13
#define PERIPH_AUDIO_DOUT_GPIO   GPIO_NUM_45
#define PERIPH_AUDIO_DIN_GPIO    GPIO_NUM_12
#define PERIPH_AUDIO_MCLK_MULT   256

static uint8_t s_volume = 80;
static bool s_initialized = false;
static bool s_tx_enabled = false;

static i2s_chan_handle_t s_tx = NULL;
static uint32_t s_sample_rate = 0;
/* ES8311 的寄存器配置会被播放任务（采样率）与 UI 任务（音量 / 静音）同时调用，串行化 */
static SemaphoreHandle_t s_cfg_mux = NULL;

/* 采样率变化时重配 I2S 时钟与 ES8311 */
static esp_err_t audio_apply_clock(uint32_t sample_rate)
{
    if (s_tx == NULL) return ESP_ERR_INVALID_STATE;
    if (sample_rate == s_sample_rate) return ESP_OK;

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk.mclk_multiple = PERIPH_AUDIO_MCLK_MULT;

    bool was_enabled = s_tx_enabled;
    if (was_enabled) {
        esp_err_t err = i2s_channel_disable(s_tx);
        if (err != ESP_OK) return err;
        s_tx_enabled = false;
    }

    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err == ESP_OK) {
        if (s_cfg_mux != NULL) xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
        err = drv_es8311_set_sample_rate(sample_rate);
        if (s_cfg_mux != NULL) xSemaphoreGive(s_cfg_mux);

        if (err == ESP_OK) {
            s_sample_rate = sample_rate;
        }
    }

    /* 失败也要把通道恢复到"可用"状态，否则后续播放会一直失败 */
    if (was_enabled) {
        esp_err_t re = i2s_channel_enable(s_tx);
        if (re != ESP_OK) {
            ESP_LOGE(TAG, "re-enable TX failed: %s", esp_err_to_name(re));
            return re;
        }
        s_tx_enabled = true;
    }
    return err;
}

esp_err_t periph_audio_init(void)
{
    if (s_initialized) return ESP_OK;

    if (s_cfg_mux == NULL) {
        s_cfg_mux = xSemaphoreCreateMutex();
        if (s_cfg_mux == NULL) return ESP_ERR_NO_MEM;
    }

    drv_pca9557_set_pin(DRV_PCA9557_PA_EN, 0);   // PA 默认关闭

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(PERIPH_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx = NULL;
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(PERIPH_AUDIO_SR_16K),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PERIPH_AUDIO_MCLK_GPIO,
            .bclk = PERIPH_AUDIO_BCLK_GPIO,
            .ws   = PERIPH_AUDIO_WS_GPIO,
            .dout = PERIPH_AUDIO_DOUT_GPIO,
            .din  = PERIPH_AUDIO_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = PERIPH_AUDIO_MCLK_MULT;

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_tx_enabled = true;
    s_sample_rate = PERIPH_AUDIO_SR_16K;

    err = drv_es8311_init(PERIPH_AUDIO_SR_16K);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed (%s)", esp_err_to_name(err));
        goto fail;
    }

    drv_es8311_set_volume(s_volume);
    drv_es8311_set_mute(false);

    s_initialized = true;
    ESP_LOGI(TAG, "initialized (I2S0, 16-bit stereo, MCLK GPIO38)");
    return ESP_OK;

fail:
    if (s_tx != NULL) {
        if (s_tx_enabled) {
            i2s_channel_disable(s_tx);
            s_tx_enabled = false;
        }
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_sample_rate = 0;
    return err;
}

esp_err_t periph_audio_set_format(periph_audio_dir_t dir, const periph_audio_format_t *fmt)
{
    if (fmt == NULL) return ESP_ERR_INVALID_ARG;

    if (dir == PERIPH_AUDIO_DIR_RECORD) {
        ESP_LOGW(TAG, "record not supported yet");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* I2S slot 固定 16-bit 立体声；单声道数据由调用方（svc_audio）复制成双声道 */
    if (fmt->bit_width != 16 || fmt->channels == 0 || fmt->channels > 2) {
        ESP_LOGE(TAG, "unsupported format: %u bit / %u ch", fmt->bit_width, fmt->channels);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = audio_apply_clock((uint32_t)fmt->sample_rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply clock failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "format set: sr=%u bits=%u ch=%u",
             (unsigned)fmt->sample_rate, fmt->bit_width, fmt->channels);
    return ESP_OK;
}

esp_err_t periph_audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;

    if (!s_initialized) return ESP_OK;

    if (s_cfg_mux != NULL) xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
    esp_err_t err = drv_es8311_set_volume(percent);
    if (s_cfg_mux != NULL) xSemaphoreGive(s_cfg_mux);
    return err;
}

uint8_t periph_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t periph_audio_set_mute(bool mute)
{
    drv_pca9557_set_pin(DRV_PCA9557_PA_EN, mute ? 0 : 1);
    if (!s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "mute: %s", mute ? "on" : "off");

    if (s_cfg_mux != NULL) xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
    esp_err_t err = drv_es8311_set_mute(mute);
    if (s_cfg_mux != NULL) xSemaphoreGive(s_cfg_mux);
    return err;
}

esp_err_t periph_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_tx == NULL) return ESP_ERR_INVALID_STATE;

    size_t written = 0;
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return i2s_channel_write(s_tx, data, len, &written, ticks);
}

esp_err_t periph_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    (void)data;
    (void)len;
    (void)timeout_ms;
    if (bytes_read) *bytes_read = 0;
    return ESP_ERR_NOT_SUPPORTED;   // 未接入 I2S RX（ES7210 录音）
}
