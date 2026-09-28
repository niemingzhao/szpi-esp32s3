/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - BSP (Board Support Package) Initialization
 *
 * 按正确顺序初始化所有底层外设，为 HAL 层提供干净的起点。
 */

#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "bsp.init";

// ============================================================================
// I2C 总线初始化 (内部使用)
// ============================================================================

static esp_err_t bsp_i2c_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_1,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = GPIO_NUM_2,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,  // 100 kHz
    };

    ESP_ERROR_CHECK(i2c_param_config(0, &i2c_conf));
    return i2c_driver_install(0, i2c_conf.mode, 0, 0, 0);
}

// ============================================================================
// BSP 初始化入口
// ============================================================================

esp_err_t bsp_init(esp_lcd_panel_handle_t *out_lcd_panel,
                    esp_lcd_panel_io_handle_t *out_lcd_io,
                    esp_lcd_touch_handle_t *out_touch)
{
    esp_lcd_panel_handle_t lcd_panel = NULL;
    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_touch_handle_t touch = NULL;

    ESP_LOGI(TAG, "=== BSP Init Start ===");

    // 1. I2C 总线 (最早，所有 I2C 设备都依赖它)
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_LOGI(TAG, "I2C0 initialized (GPIO1=SDA, GPIO2=SCL, 100kHz)");

    // 2. LEDC (背光 PWM，在 SPI LCD 之前初始化)
    ESP_ERROR_CHECK(drv_ledc_init());
    ESP_LOGI(TAG, "LEDC initialized");

    // 3. PCA9557 IO 扩展 (LCD_CS/PA_EN/DVP_PWDN)
    ESP_ERROR_CHECK(drv_pca9557_init());
    ESP_LOGI(TAG, "PCA9557 initialized");

    // 4. ST7789 LCD 面板 (SPI3_HOST)
    ESP_ERROR_CHECK(drv_st7789_init(true));  // true = CS 由 PCA9557 控制
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

    // 获取各外设 handle
    ESP_ERROR_CHECK(drv_st7789_get_panel_handle(&lcd_panel));
    ESP_ERROR_CHECK(drv_st7789_get_io_handle(&lcd_io));
    ESP_ERROR_CHECK(drv_ft6336_get_touch_handle(&touch));

    // 通过输出参数返回
    if (out_lcd_panel) *out_lcd_panel = lcd_panel;
    if (out_lcd_io) *out_lcd_io = lcd_io;
    if (out_touch) *out_touch = touch;

    ESP_LOGI(TAG, "=== BSP Init Done ===");
    return ESP_OK;
}
