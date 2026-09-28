/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - LCD
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 屏幕旋转方向
 */
typedef enum {
    PERIPH_LCD_ROT_0 = 0,
    PERIPH_LCD_ROT_90,
    PERIPH_LCD_ROT_180,
    PERIPH_LCD_ROT_270,
} periph_lcd_rotation_t;

/**
 * @brief 初始化 LCD + LVGL display
 */
esp_err_t periph_lcd_init(void);

/**
 * @brief 获取 LVGL display 对象（给 framework）
 */
lv_disp_t *periph_lcd_get_disp(void);

/**
 * @brief 获取屏幕分辨率
 */
uint16_t periph_lcd_get_width(void);
uint16_t periph_lcd_get_height(void);

/**
 * @brief 设置亮度 0-100（自动 NVS 持久化）
 */
esp_err_t periph_lcd_set_brightness(uint8_t percent);

/**
 * @brief 获取当前亮度
 */
uint8_t periph_lcd_get_brightness(void);

/**
 * @brief 背光开关
 */
esp_err_t periph_lcd_set_backlight(bool on);

/**
 * @brief 屏幕旋转
 */
esp_err_t periph_lcd_set_rotation(periph_lcd_rotation_t rot);

/**
 * @brief 全屏颜色填充（仅供 LVGL 接管前使用）
 */
void periph_lcd_fill(uint16_t color);

/**
 * @brief 画图（从 RAM，仅供 LVGL 接管前使用）
 */
esp_err_t periph_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t *buf);

#ifdef __cplusplus
}
#endif
