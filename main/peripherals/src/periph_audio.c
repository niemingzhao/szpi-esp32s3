/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Audio (ES8311 DAC + I2S)
 *
 * - 控制：ES8311 走 I2C0（drv_es8311）
 * - 数据：I2S0 发送通道，MCLK GPIO38 / BCLK GPIO14 / WS GPIO13 / DOUT GPIO45
 * - 功放：PA_EN 由 PCA9557.BIT1 控制（静音时关闭）
 * - 录音：ES7210 走 I2S0 接收通道（TDM 2 slot）
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
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
static i2s_chan_handle_t s_rx = NULL;      /* ES7210 采集（TDM 2 slot） */
static uint32_t s_sample_rate = 0;
static bool s_rx_enabled = false;
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

/* 开机采集自检：读一小段麦克风数据算峰值，日志里就能看出采集通路是否正常 */
static void mic_selftest(void)
{
    int16_t buf[512];
    size_t rd = 0;

    if (s_rx == NULL) return;

    esp_err_t err = i2s_channel_read(s_rx, buf, sizeof(buf), &rd, pdMS_TO_TICKS(200));
    if (err != ESP_OK || rd == 0) {
        ESP_LOGW(TAG, "mic selftest: read failed (%s)", esp_err_to_name(err));
        return;
    }

    int32_t peak = 0;
    size_t samples = rd / sizeof(int16_t);
    for (size_t i = 0; i < samples; i++) {
        int32_t v = buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    ESP_LOGI(TAG, "mic selftest: %u samples, peak %d/32767", (unsigned)samples, (int)peak);
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
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
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

    /* RX：TDM 2 slot，从 ES7210 采集（板载 2 路 MIC + 1 路 ES8311 回环占 slot0/1） */
    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = PERIPH_AUDIO_SR_16K,
            .mclk_multiple = PERIPH_AUDIO_MCLK_MULT,
        },
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .gpio_cfg = {
            .mclk = PERIPH_AUDIO_MCLK_GPIO,
            .bclk = PERIPH_AUDIO_BCLK_GPIO,
            .ws   = PERIPH_AUDIO_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = PERIPH_AUDIO_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_tdm_mode(s_rx, &tdm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_tdm_mode failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 先使能 RX（ES7210 需要 BCLK/WS 才输出数据），再使能 TX */
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(rx) failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_rx_enabled = true;

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable(tx) failed: %s", esp_err_to_name(err));
        goto fail;
    }
    s_tx_enabled = true;
    s_sample_rate = PERIPH_AUDIO_SR_16K;

    err = drv_es7210_init(PERIPH_AUDIO_SR_16K);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES7210 init failed (%s)", esp_err_to_name(err));
        goto fail;
    }

    err = drv_es8311_init(PERIPH_AUDIO_SR_16K);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init failed (%s)", esp_err_to_name(err));
        goto fail;
    }

    drv_es8311_set_volume(s_volume);
    drv_es8311_set_mute(false);

    mic_selftest();

    s_initialized = true;
    ESP_LOGI(TAG, "initialized (I2S0, 16-bit stereo, MCLK GPIO38)");
    return ESP_OK;

fail:
    if (s_rx != NULL) {
        if (s_rx_enabled) {
            i2s_channel_disable(s_rx);
            s_rx_enabled = false;
        }
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
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
        /* ES7210 只支持 16 / 44.1 / 48 kHz */
        if (fmt->sample_rate != PERIPH_AUDIO_SR_16K &&
            fmt->sample_rate != PERIPH_AUDIO_SR_44K &&
            fmt->sample_rate != PERIPH_AUDIO_SR_48K) {
            ESP_LOGE(TAG, "record sample rate not supported: %u", (unsigned)fmt->sample_rate);
            return ESP_ERR_NOT_SUPPORTED;
        }

        /* 录音：RX 固定 16-bit 立体声，只需切采样率（同一条 I2S 总线，时钟两边一起更） */
        if (fmt->bit_width != 16 || fmt->channels == 0 || fmt->channels > 2) {
            ESP_LOGE(TAG, "unsupported record format: %u bit / %u ch", fmt->bit_width, fmt->channels);
            return ESP_ERR_NOT_SUPPORTED;
        }

        esp_err_t rerr = audio_apply_clock((uint32_t)fmt->sample_rate);
        if (rerr == ESP_OK && s_rx != NULL) {
            i2s_tdm_clk_config_t tdm_clk = I2S_TDM_CLK_DEFAULT_CONFIG((uint32_t)fmt->sample_rate);
            tdm_clk.mclk_multiple = PERIPH_AUDIO_MCLK_MULT;
            rerr = i2s_channel_reconfig_tdm_clock(s_rx, &tdm_clk);
        }
        if (rerr == ESP_OK) {
            if (s_cfg_mux != NULL) xSemaphoreTake(s_cfg_mux, portMAX_DELAY);
            rerr = drv_es7210_set_sample_rate((uint32_t)fmt->sample_rate);
            if (s_cfg_mux != NULL) xSemaphoreGive(s_cfg_mux);
        }

        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "record format failed: %s", esp_err_to_name(rerr));
            return rerr;
        }

        ESP_LOGI(TAG, "record format set: sr=%u bits=%u ch=%u",
                 (unsigned)fmt->sample_rate, fmt->bit_width, fmt->channels);
        return ESP_OK;
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
    esp_err_t err = i2s_channel_write(s_tx, data, len, &written, ticks);
    if (err == ESP_OK && written != len) {
        ESP_LOGE(TAG, "short write: %u/%u bytes", (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    return err;
}

esp_err_t periph_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    if (bytes_read) *bytes_read = 0;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_rx == NULL) return ESP_ERR_INVALID_STATE;

    size_t rd = 0;
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    esp_err_t err = i2s_channel_read(s_rx, data, len, &rd, ticks);
    if (bytes_read) *bytes_read = rd;
    return err;
}
