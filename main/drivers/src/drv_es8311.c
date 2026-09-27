/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drivers - ES8311 音频 DAC（I2C0 @ 0x18）
 *
 * 只负责 I2C 侧配置；I2S 数据通路由 Peripherals 层 periph_audio 负责。
 *
 * 寄存器序列按手册整理，不引入第三方 codec 组件。
 *
 * 时钟拓扑固定为板上的接法：MCLK 由 I2S 主机从 MCLK 引脚提供、无反转、
 * MCLK = 采样率 × 256、SDP 输入/输出均为 16 bit I2S 从机。该比例下 ES8311
 * 的分频系数与采样率无关（pre_div / adc_div / dac_div / bclk_div 固定），
 * 因此只需要一张系数表。寄存器定义见 ES8311 数据手册。
 */

#include "drv_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "drv.es8311";

#define DRV_ES8311_ADDR             0x18
#define DRV_ES8311_MCLK_MULTIPLE    256

/* 寄存器（编号见 ES8311 数据手册） */
#define DRV_ES8311_REG_RESET        0x00    /* 复位 / 主从模式 */
#define DRV_ES8311_REG_CLK_SRC      0x01    /* MCLK 时钟源选择与时钟使能 */
#define DRV_ES8311_REG_CLK_DIV_PRE  0x02    /* 预分频、预倍频 */
#define DRV_ES8311_REG_ADC_FS_OSR   0x03    /* ADC fs mode 与 osr */
#define DRV_ES8311_REG_DAC_OSR      0x04    /* DAC osr */
#define DRV_ES8311_REG_CLK_DIV_AD   0x05    /* ADC / DAC 分频 */
#define DRV_ES8311_REG_BCLK_DIV     0x06    /* BCLK 反相与分频 */
#define DRV_ES8311_REG_LRCK_H       0x07    /* LRCK 分频高位 */
#define DRV_ES8311_REG_LRCK_L       0x08    /* LRCK 分频低位 */
#define DRV_ES8311_REG_SDP_IN       0x09    /* DAC 串行数字口 */
#define DRV_ES8311_REG_SDP_OUT      0x0A    /* ADC 串行数字口 */
#define DRV_ES8311_REG_SYS_PWR1     0x0D    /* 模拟电路上电 */
#define DRV_ES8311_REG_SYS_PWR2     0x0E    /* 模拟 PGA / ADC 调制器 */
#define DRV_ES8311_REG_SYS_DAC_PWR  0x12    /* DAC 上电 */
#define DRV_ES8311_REG_SYS_HP_DRV   0x13    /* HP 驱动使能 */
#define DRV_ES8311_REG_SYS_MIC      0x14    /* MIC 选择与 PGA 增益 */
#define DRV_ES8311_REG_ADC_VOL      0x17    /* ADC 音量 */
#define DRV_ES8311_REG_ADC_EQ       0x1C    /* ADC 均衡器 */
#define DRV_ES8311_REG_DAC_MUTE     0x31    /* DAC 静音 */
#define DRV_ES8311_REG_DAC_VOL      0x32    /* DAC 音量 */
#define DRV_ES8311_REG_DAC_EQ       0x37    /* DAC 均衡器 */

/* MCLK = 采样率 × 256 时的分频系数（与采样率无关） */
#define DRV_ES8311_PRE_DIV      1
#define DRV_ES8311_PRE_MULTI    0
#define DRV_ES8311_ADC_DIV      1
#define DRV_ES8311_DAC_DIV      1
#define DRV_ES8311_FS_MODE      0
#define DRV_ES8311_LRCK_H       0x00
#define DRV_ES8311_LRCK_L       0xFF
#define DRV_ES8311_BCLK_DIV     4
#define DRV_ES8311_ADC_OSR      0x10
#define DRV_ES8311_DAC_OSR      0x10

/* 16 bit 分辨率在 SDP 寄存器里的编码：(3 << 2) */
#define DRV_ES8311_SDP_16BIT    0x0C

static i2c_master_dev_handle_t s_dev = NULL;
static uint32_t s_sample_rate = 0;
static bool s_initialized = false;

/* 与 MCLK = 采样率 × 256 匹配、且在系数表内的采样率 */
static bool rate_supported(uint32_t rate)
{
    switch (rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
    case 64000:
        return true;
    default:
        return false;
    }
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    return drv_i2c_write_reg(s_dev, reg, &value, 1);
}

/* 读-改-写：保留 keep_mask 指定的位，其余位置为 set_bits */
static esp_err_t reg_update(uint8_t reg, uint8_t keep_mask, uint8_t set_bits)
{
    uint8_t value = 0;
    esp_err_t err = drv_i2c_read_reg(s_dev, reg, &value, 1);
    if (err != ESP_OK) {
        return err;
    }
    return reg_write(reg, (uint8_t)((value & keep_mask) | set_bits));
}

/* 写入时钟分频（reg02 ~ reg08） */
static esp_err_t clock_div_config(void)
{
    esp_err_t err;

    err = reg_update(DRV_ES8311_REG_CLK_DIV_PRE, 0x07,
                     (uint8_t)(((DRV_ES8311_PRE_DIV - 1) << 5) | (DRV_ES8311_PRE_MULTI << 3)));
    if (err != ESP_OK) return err;

    err = reg_write(DRV_ES8311_REG_ADC_FS_OSR, (uint8_t)((DRV_ES8311_FS_MODE << 6) | DRV_ES8311_ADC_OSR));
    if (err != ESP_OK) return err;

    err = reg_write(DRV_ES8311_REG_DAC_OSR, DRV_ES8311_DAC_OSR);
    if (err != ESP_OK) return err;

    err = reg_write(DRV_ES8311_REG_CLK_DIV_AD,
                    (uint8_t)(((DRV_ES8311_ADC_DIV - 1) << 4) | (DRV_ES8311_DAC_DIV - 1)));
    if (err != ESP_OK) return err;

    err = reg_update(DRV_ES8311_REG_BCLK_DIV, 0xE0, (uint8_t)(DRV_ES8311_BCLK_DIV - 1));
    if (err != ESP_OK) return err;

    err = reg_update(DRV_ES8311_REG_LRCK_H, 0xC0, DRV_ES8311_LRCK_H);
    if (err != ESP_OK) return err;

    return reg_write(DRV_ES8311_REG_LRCK_L, DRV_ES8311_LRCK_L);
}

/* 串行口：I2S 从机、16 bit */
static esp_err_t sdp_config(void)
{
    /* 清 bit6 → 从机模式 */
    esp_err_t err = reg_update(DRV_ES8311_REG_RESET, (uint8_t)~0x40, 0x00);
    if (err != ESP_OK) return err;

    err = reg_write(DRV_ES8311_REG_SDP_IN, DRV_ES8311_SDP_16BIT);
    if (err != ESP_OK) return err;
    return reg_write(DRV_ES8311_REG_SDP_OUT, DRV_ES8311_SDP_16BIT);
}

esp_err_t drv_es8311_init(uint32_t sample_rate)
{
    if (!rate_supported(sample_rate)) {
        ESP_LOGE(TAG, "unsupported sample rate %u Hz (MCLK would be %u Hz)",
                 (unsigned)sample_rate, (unsigned)(sample_rate * DRV_ES8311_MCLK_MULTIPLE));
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        return drv_es8311_set_sample_rate(sample_rate);
    }

    if (s_dev == NULL) {
        esp_err_t derr = drv_i2c_device_add(DRV_ES8311_ADDR, DRV_I2C_FREQ_HZ, &s_dev);
        if (derr != ESP_OK) {
            ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(derr));
            return derr;
        }
    }

    /* 复位并上电 */
    esp_err_t err = reg_write(DRV_ES8311_REG_RESET, 0x1F);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    err = reg_write(DRV_ES8311_REG_RESET, 0x00);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_RESET, 0x80);
    if (err != ESP_OK) return err;

    /* 时钟：所有时钟开启，MCLK 取自 MCLK 引脚，不反转 SCLK */
    err = reg_write(DRV_ES8311_REG_CLK_SRC, 0x3F);
    if (err != ESP_OK) return err;
    err = reg_update(DRV_ES8311_REG_BCLK_DIV, (uint8_t)~0x20, 0x00);
    if (err != ESP_OK) return err;

    err = clock_div_config();
    if (err != ESP_OK) return err;
    err = sdp_config();
    if (err != ESP_OK) return err;

    /* 模拟部分上电与默认值修正 */
    err = reg_write(DRV_ES8311_REG_SYS_PWR1, 0x01);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_SYS_PWR2, 0x02);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_SYS_DAC_PWR, 0x00);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_SYS_HP_DRV, 0x10);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_ADC_EQ, 0x6A);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_DAC_EQ, 0x08);
    if (err != ESP_OK) return err;

    /* 模拟 MIC + 最大 PGA（本机只放音，保持与原组件一致） */
    err = reg_write(DRV_ES8311_REG_ADC_VOL, 0xC8);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES8311_REG_SYS_MIC, 0x1A);
    if (err != ESP_OK) return err;

    s_sample_rate = sample_rate;
    s_initialized = true;
    ESP_LOGI(TAG, "initialized (%u Hz, MCLK %u Hz)",
             (unsigned)sample_rate, (unsigned)(sample_rate * DRV_ES8311_MCLK_MULTIPLE));
    return ESP_OK;
}

esp_err_t drv_es8311_set_sample_rate(uint32_t sample_rate)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (!rate_supported(sample_rate)) return ESP_ERR_INVALID_ARG;
    if (sample_rate == s_sample_rate) return ESP_OK;

    esp_err_t err = clock_div_config();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set sample rate %u failed: %s",
                 (unsigned)sample_rate, esp_err_to_name(err));
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

    /* 0 视为静音；否则 DAC 音量寄存器 = 音量 × 256 / 100 - 1 */
    uint8_t reg32 = (percent == 0) ? 0 : (uint8_t)((percent * 256 / 100) - 1);

    esp_err_t err = reg_write(DRV_ES8311_REG_DAC_VOL, reg32);
    if (err != ESP_OK) return err;

    /* 这里不打日志：音量滑块每走一格都会调进来，和亮度一样只在初始化时打一次
     * （见 periph_audio_init / svc_audio_init） */
    return ESP_OK;
}

esp_err_t drv_es8311_set_mute(bool mute)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;

    /* reg31 bit6/bit5 = DAC 静音 */
    const uint8_t mute_bits = 0x60;

    return reg_update(DRV_ES8311_REG_DAC_MUTE, (uint8_t)~mute_bits, mute ? mute_bits : 0x00);
}
