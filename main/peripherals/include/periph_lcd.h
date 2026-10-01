/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - LCD
 *
 * LCD 面板的硬件初始化由 Drivers 层 bsp_init() 完成，本模块只负责 LVGL 集成
 * 与业务级亮度接口（亮度的持久化由 Services 层 svc_power 负责）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LCD + LVGL display
 */
esp_err_t periph_lcd_init(void);

/**
 * @brief 获取 LVGL display 对象（给 framework）—— 唯一向 Framework 暴露的 LVGL 对象
 */
lv_display_t *periph_lcd_get_disp(void);

/**
 * @brief 获取屏幕分辨率
 */
uint16_t periph_lcd_get_width(void);
uint16_t periph_lcd_get_height(void);

/**
 * @brief 设置亮度 0-100（持久化由 svc_power 负责）
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
