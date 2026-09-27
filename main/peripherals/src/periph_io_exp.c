/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IO 扩展封装（PCA9557）
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "periph.io_exp";

esp_err_t periph_io_exp_init(void)
{
    esp_err_t err = drv_pca9557_init();
    if (err == ESP_OK) ESP_LOGI(TAG, "initialized");
    return err;
}

esp_err_t periph_io_exp_set(periph_io_pin_t pin, periph_io_level_t level)
{
    uint8_t bit;
    switch (pin) {
        case PERIPH_IO_LCD_CS:   bit = DRV_PCA9557_LCD_CS;   break;
        case PERIPH_IO_PA_EN:    bit = DRV_PCA9557_PA_EN;    break;
        case PERIPH_IO_DVP_PWDN: bit = DRV_PCA9557_DVP_PWDN; break;
        default:                 return ESP_ERR_INVALID_ARG;
    }
    return drv_pca9557_set_pin(bit, level == PERIPH_IO_LEVEL_HIGH ? 1 : 0);
}
