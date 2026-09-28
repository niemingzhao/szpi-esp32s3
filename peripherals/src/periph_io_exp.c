/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IO Expander (wraps PCA9557 driver)
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "periph.io_exp";

esp_err_t periph_io_exp_init(void)
{
    return drv_pca9557_init();
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

periph_io_level_t periph_io_exp_get(periph_io_pin_t pin)
{
    uint8_t bit;
    switch (pin) {
        case PERIPH_IO_LCD_CS:   bit = DRV_PCA9557_LCD_CS;   break;
        case PERIPH_IO_PA_EN:    bit = DRV_PCA9557_PA_EN;    break;
        case PERIPH_IO_DVP_PWDN: bit = DRV_PCA9557_DVP_PWDN; break;
        default:                 return PERIPH_IO_LEVEL_LOW;
    }
    uint8_t out = 0;
    drv_pca9557_get_pin(bit, &out);
    return out ? PERIPH_IO_LEVEL_HIGH : PERIPH_IO_LEVEL_LOW;
}
