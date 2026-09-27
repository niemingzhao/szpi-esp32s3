/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drivers - ES7210 四通道音频 ADC（I2C0 @ 0x41）
 *
 * 只负责 I2C 侧配置；I2S 数据通路由 Peripherals 层 periph_audio 的 RX 通道负责。
 *
 * 寄存器序列按手册整理，不引入第三方 codec 组件。
 *
 * 时钟拓扑固定为板上的接法：MCLK 由 I2S 主机提供、MCLK = 采样率 × 256、16-bit、
 * I2S 格式 + 1xFS TDM。该比例下分频系数与采样率无关（osr/adc_div/dll/doubler/lrck
 * 固定），因此只有一组系数；板载 2 路 MIC + 1 路 ES8311 回环走 ES7210 的 4 通道（实际用 3 路）。
 * 寄存器定义见 ES7210 数据手册。
 */

#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "drv.es7210";

#define DRV_ES7210_ADDR             0x41
#define DRV_ES7210_MCLK_MULTIPLE    256

/* 寄存器（编号见 ES7210 数据手册） */
#define DRV_ES7210_REG_RESET        0x00    /* 复位 / 使能 */
#define DRV_ES7210_REG_MAINCLK      0x02    /* adc_div / doubler / dll */
#define DRV_ES7210_REG_LRCK_DIVH    0x04
#define DRV_ES7210_REG_LRCK_DIVL    0x05
#define DRV_ES7210_REG_POWER_DOWN   0x06    /* 关闭 DLL */
#define DRV_ES7210_REG_OSR          0x07
#define DRV_ES7210_REG_TIME0        0x09    /* 上电初始化时间 */
#define DRV_ES7210_REG_TIME1        0x0A
#define DRV_ES7210_REG_SDP_INT1     0x11    /* 采样位宽 / 格式 */
#define DRV_ES7210_REG_SDP_INT2     0x12    /* 引脚状态 / TDM */
#define DRV_ES7210_REG_ADC34_HPF2   0x20
#define DRV_ES7210_REG_ADC34_HPF1   0x21
#define DRV_ES7210_REG_ADC12_HPF2   0x22
#define DRV_ES7210_REG_ADC12_HPF1   0x23
#define DRV_ES7210_REG_ANALOG       0x40
#define DRV_ES7210_REG_MIC12_BIAS   0x41
#define DRV_ES7210_REG_MIC34_BIAS   0x42
#define DRV_ES7210_REG_MIC1_GAIN    0x43    /* 43~46 依次为 MIC1~4 增益 */
#define DRV_ES7210_REG_MIC1_POWER   0x47    /* 47~4A 依次为 MIC1~4 上电 */
#define DRV_ES7210_REG_MIC12_POWER  0x4B    /* MICbias & ADC & PGA 上电 */
#define DRV_ES7210_REG_MIC34_POWER  0x4C

/* 16-bit / I2S 格式在 SDP 寄存器里的编码 */
#define DRV_ES7210_SDP_16BIT_I2S    0x60
/* 1xFS TDM 使能 */
#define DRV_ES7210_SDP_TDM_EN       0x02
/* MIC 增益 30 dB、bias 2.87 V、模拟上电值 */
#define DRV_ES7210_MIC_GAIN_30DB    0x0A
#define DRV_ES7210_MIC_BIAS_2V87    0x70
/* MCLK = 采样率 × 256 时的分频系数（与采样率无关） */
#define DRV_ES7210_OSR_VALUE        0x20
#define DRV_ES7210_MAINCLK_VALUE    (0x01 | (0x01 << 6) | (0x01 << 7))   /* adc_div | doubler | dll */
#define DRV_ES7210_LRCK_DIVH_VALUE  0x01
#define DRV_ES7210_LRCK_DIVL_VALUE  0x00

static i2c_master_dev_handle_t s_dev = NULL;
static uint32_t s_sample_rate = 0;
static bool s_initialized = false;

/* 与 MCLK = 采样率 × 256 匹配、且在 ES7210 系数表内的采样率 */
static bool rate_supported(uint32_t rate)
{
    switch (rate) {
    case 16000:
    case 44100:
    case 48000:
        return true;
    default:
        return false;
    }
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    return drv_i2c_write_reg(s_dev, reg, &value, 1);
}

/* 写入时钟分频（reg02 / reg04 / reg05 / reg07） */
static esp_err_t clock_div_config(void)
{
    esp_err_t err;

    err = reg_write(DRV_ES7210_REG_OSR, DRV_ES7210_OSR_VALUE);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_MAINCLK, DRV_ES7210_MAINCLK_VALUE);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_LRCK_DIVH, DRV_ES7210_LRCK_DIVH_VALUE);
    if (err != ESP_OK) return err;
    return reg_write(DRV_ES7210_REG_LRCK_DIVL, DRV_ES7210_LRCK_DIVL_VALUE);
}

esp_err_t drv_es7210_init(uint32_t sample_rate)
{
    if (!rate_supported(sample_rate)) {
        ESP_LOGE(TAG, "unsupported sample rate %u Hz (MCLK would be %u Hz)",
                 (unsigned)sample_rate, (unsigned)(sample_rate * DRV_ES7210_MCLK_MULTIPLE));
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        return drv_es7210_set_sample_rate(sample_rate);
    }

    if (s_dev == NULL) {
        esp_err_t derr = drv_i2c_device_add(DRV_ES7210_ADDR, DRV_I2C_FREQ_HZ, &s_dev);
        if (derr != ESP_OK) {
            ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(derr));
            return derr;
        }
    }

    esp_err_t err;

    /* 软件复位 + 上电初始化时间 */
    err = reg_write(DRV_ES7210_REG_RESET, 0xFF);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_RESET, 0x32);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_TIME0, 0x30);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_TIME1, 0x30);
    if (err != ESP_OK) return err;

    /* ADC1~4 高通滤波 */
    err = reg_write(DRV_ES7210_REG_ADC12_HPF1, 0x2A);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_ADC12_HPF2, 0x0A);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_ADC34_HPF1, 0x2A);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_ADC34_HPF2, 0x0A);
    if (err != ESP_OK) return err;

    /* 16-bit、I2S 格式、1xFS TDM */
    err = reg_write(DRV_ES7210_REG_SDP_INT1, DRV_ES7210_SDP_16BIT_I2S);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_SDP_INT2, DRV_ES7210_SDP_TDM_EN);
    if (err != ESP_OK) return err;

    /* 模拟部分上电 + MIC bias / 增益 */
    err = reg_write(DRV_ES7210_REG_ANALOG, 0xC3);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_MIC12_BIAS, DRV_ES7210_MIC_BIAS_2V87);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_MIC34_BIAS, DRV_ES7210_MIC_BIAS_2V87);
    if (err != ESP_OK) return err;
    for (uint8_t reg = DRV_ES7210_REG_MIC1_GAIN; reg <= DRV_ES7210_REG_MIC1_GAIN + 3; reg++) {
        err = reg_write(reg, DRV_ES7210_MIC_GAIN_30DB | 0x10);
        if (err != ESP_OK) return err;
    }
    for (uint8_t reg = DRV_ES7210_REG_MIC1_POWER; reg <= DRV_ES7210_REG_MIC1_POWER + 3; reg++) {
        err = reg_write(reg, 0x08);
        if (err != ESP_OK) return err;
    }

    /* 时钟分频 */
    err = clock_div_config();
    if (err != ESP_OK) return err;

    /* 关 DLL → MIC bias / ADC / PGA 上电 → 使能器件 */
    err = reg_write(DRV_ES7210_REG_POWER_DOWN, 0x04);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_MIC12_POWER, 0x0F);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_MIC34_POWER, 0x0F);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_RESET, 0x71);
    if (err != ESP_OK) return err;
    err = reg_write(DRV_ES7210_REG_RESET, 0x41);
    if (err != ESP_OK) return err;

    s_sample_rate = sample_rate;
    s_initialized = true;
    ESP_LOGI(TAG, "initialized (%u Hz, MCLK %u Hz, MIC gain 30 dB)",
             (unsigned)sample_rate, (unsigned)(sample_rate * DRV_ES7210_MCLK_MULTIPLE));
    return ESP_OK;
}

esp_err_t drv_es7210_set_sample_rate(uint32_t sample_rate)
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
