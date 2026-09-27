/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - PCA9557 IO 扩展（I2C 0x19，控制 LCD_CS / PA_EN / DVP_PWDN）
 */

#include "drv_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "drv.pca9557";

#define DRV_PCA9557_ADDR         0x19

/* 上电默认输出：DVP_PWDN=1(掉电) | LCD_CS=1(未选中) | PA_EN=0(关闭功放) */
#define DRV_PCA9557_OUT_DEFAULT  0x05

/* PCA9557 寄存器 */
#define DRV_PCA9557_OUTPUT_PORT   0x01
#define DRV_PCA9557_CONFIG_PORT   0x03

static bool s_initialized = false;
static i2c_master_dev_handle_t s_dev = NULL;
/* 输出寄存器影子：省掉一次 I2C 读。s_lock 把影子更新与总线写入串起来 —— 相机（DVP_PWDN）
 * 与音频（PA_EN）在两个任务里各改各的位，不加锁会丢更新 */
static uint8_t s_out_shadow = DRV_PCA9557_OUT_DEFAULT;
static SemaphoreHandle_t s_lock = NULL;

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t data)
{
    return drv_i2c_write_reg(s_dev, reg, &data, 1);
}

esp_err_t drv_pca9557_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(drv_i2c_device_add(DRV_PCA9557_ADDR, DRV_I2C_FREQ_HZ, &s_dev));

    // BIT0/BIT1/BIT2 设为输出，其他保持输入
    ESP_ERROR_CHECK(pca9557_write_reg(DRV_PCA9557_CONFIG_PORT, 0xF8));

    // 默认值: DVP_PWDN=1(掉电), LCD_CS=1(未选中), PA_EN=0(关闭)
    s_out_shadow = DRV_PCA9557_OUT_DEFAULT;
    ESP_ERROR_CHECK(pca9557_write_reg(DRV_PCA9557_OUTPUT_PORT, s_out_shadow));

    s_initialized = true;
    ESP_LOGI(TAG, "PCA9557 initialized (BIT0=CS, BIT1=PA_EN, BIT2=DVP_PWDN)");
    return ESP_OK;
}

esp_err_t drv_pca9557_set_pin(uint8_t gpio_bit, uint8_t level)
{
    if (gpio_bit & ~0x07) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (level) {
        s_out_shadow |= gpio_bit;
    } else {
        s_out_shadow &= ~gpio_bit;
    }

    const esp_err_t err = pca9557_write_reg(DRV_PCA9557_OUTPUT_PORT, s_out_shadow);
    xSemaphoreGive(s_lock);
    return err;
}
