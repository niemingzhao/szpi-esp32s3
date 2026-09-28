/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IO Expander (PCA9557)
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IO 引脚
 */
typedef enum {
    PERIPH_IO_LCD_CS,
    PERIPH_IO_PA_EN,
    PERIPH_IO_DVP_PWDN,
} periph_io_pin_t;

/**
 * @brief 电平
 */
typedef enum {
    PERIPH_IO_LEVEL_LOW = 0,
    PERIPH_IO_LEVEL_HIGH = 1,
} periph_io_level_t;

/**
 * @brief 初始化 IO 扩展（PCA9557）
 */
esp_err_t periph_io_exp_init(void);

/**
 * @brief 设置引脚电平
 */
esp_err_t periph_io_exp_set(periph_io_pin_t pin, periph_io_level_t level);

/**
 * @brief 读取引脚电平
 */
periph_io_level_t periph_io_exp_get(periph_io_pin_t pin);

#ifdef __cplusplus
}
#endif
