/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - FT6336 Touch Controller Driver
 */

#include "drv_common.h"
#include "esp_lcd_touch_ft5x06.h"

static const char *TAG = "drv.ft6336";

static esp_lcd_touch_handle_t s_touch_handle = NULL;
static bool s_initialized = false;

esp_err_t drv_ft6336_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // I2C panel IO (legacy v1 API，不需要也不允许设置 scl_speed_hz)
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)0, &tp_io_config, &tp_io_handle));

    // 触摸配置 - 坐标与 LCD 匹配（已交换 XY + 镜像 X）
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = DRV_LCD_V_RES,  // 240
        .y_max = DRV_LCD_H_RES,  // 320
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch_handle));

    s_initialized = true;
    ESP_LOGI(TAG, "FT6336 initialized (I2C @ 0x38, poll mode)");
    return ESP_OK;
}

esp_err_t drv_ft6336_get_touch_handle(esp_lcd_touch_handle_t *out_handle)
{
    if (s_touch_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_handle = s_touch_handle;
    return ESP_OK;
}
