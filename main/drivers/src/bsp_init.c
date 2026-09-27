/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - BSP 板级初始化
 * 按固定顺序初始化底层外设，句柄由各驱动的 get_*_handle() 提供，供 Peripherals 层使用。
 */

#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "bsp.init";

/* --------------------------- BSP 初始化入口 --------------------------- */

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG, "=== BSP Init Start ===");

    // 1. I2C 总线 (最早，所有 I2C 设备都依赖它；日志由 drv_i2c 打印)
    ESP_ERROR_CHECK(drv_i2c_bus_init());

    // 2. LEDC (背光 PWM，在 SPI LCD 之前初始化)
    ESP_ERROR_CHECK(drv_ledc_init());
    ESP_LOGI(TAG, "LEDC initialized");

    // 3. PCA9557 IO 扩展 (LCD_CS/PA_EN/DVP_PWDN)
    ESP_ERROR_CHECK(drv_pca9557_init());
    ESP_LOGI(TAG, "PCA9557 initialized");

    // 4. ST7789 LCD 面板 (SPI3_HOST)
    ESP_ERROR_CHECK(drv_st7789_init());  // CS 由 PCA9557.BIT0 控制
    ESP_LOGI(TAG, "ST7789 initialized");

    // 5. FT6336 触摸 (I2C0)
    ESP_ERROR_CHECK(drv_ft6336_init());
    ESP_LOGI(TAG, "FT6336 initialized");

    // 6. BOOT 按键 (GPIO0)
    ESP_ERROR_CHECK(drv_key_init());
    ESP_LOGI(TAG, "BOOT key initialized");

    // 7. QMI8658 IMU (可选，失败不阻塞启动)
    if (drv_qmi8658_init() == ESP_OK) {
        ESP_LOGI(TAG, "QMI8658 initialized");
    } else {
        ESP_LOGW(TAG, "QMI8658 init failed, IMU unavailable");
    }

    ESP_LOGI(TAG, "=== BSP Init Done ===");
    return ESP_OK;
}
