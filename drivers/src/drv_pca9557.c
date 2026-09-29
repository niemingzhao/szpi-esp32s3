/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - PCA9557 IO Expander Driver
 */

#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "drv.pca9557";

#define PCA9557_ADDR         0x19

// PCA9557 寄存器
#define PCA9557_INPUT_PORT    0x00
#define PCA9557_OUTPUT_PORT   0x01
#define PCA9557_POL_INV       0x02
#define PCA9557_CONFIG_PORT    0x03

static bool s_initialized = false;
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *data)
{
    return drv_i2c_read_reg(s_dev, reg, data, 1);
}

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t data)
{
    return drv_i2c_write_reg(s_dev, reg, &data, 1);
}

esp_err_t drv_pca9557_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(drv_i2c_device_add(PCA9557_ADDR, DRV_I2C_FREQ_HZ, &s_dev));

    // BIT0/BIT1/BIT2 设为输出，其他保持输入
    ESP_ERROR_CHECK(pca9557_write_reg(PCA9557_CONFIG_PORT, 0xF8));

    // 默认值: DVP_PWDN=1(掉电), LCD_CS=1(未选中), PA_EN=0(关闭)
    ESP_ERROR_CHECK(pca9557_write_reg(PCA9557_OUTPUT_PORT, 0x05));

    s_initialized = true;
    ESP_LOGI(TAG, "PCA9557 initialized (BIT0=CS, BIT1=PA_EN, BIT2=DVP_PWDN)");
    return ESP_OK;
}

esp_err_t drv_pca9557_set_pin(uint8_t gpio_bit, uint8_t level)
{
    uint8_t data;
    ESP_ERROR_CHECK(pca9557_read_reg(PCA9557_OUTPUT_PORT, &data));

    if (level) {
        data |= gpio_bit;
    } else {
        data &= ~gpio_bit;
    }

    return pca9557_write_reg(PCA9557_OUTPUT_PORT, data);
}

esp_err_t drv_pca9557_get_pin(uint8_t gpio_bit, uint8_t *out_level)
{
    uint8_t data;
    ESP_ERROR_CHECK(pca9557_read_reg(PCA9557_INPUT_PORT, &data));
    *out_level = (data & gpio_bit) ? 1 : 0;
    return ESP_OK;
}
