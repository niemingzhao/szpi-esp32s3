/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS Drivers Layer - Common Header
 * 驱动层公共头文件，声明所有芯片级驱动的初始化接口
 */

#pragma once

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BSP - Board Initialization
// ============================================================================

/**
 * @brief 板级初始化 - 按正确顺序初始化所有底层外设
 *
 * 调用顺序: I2C → SPI bus → PCA9557 → LEDC backlight
 *
 * @param out_lcd_panel 输出: LCD panel handle（供 LVGL 使用）
 * @param out_lcd_io    输出: LCD panel IO handle（供 LVGL 使用）
 * @param out_touch     输出: Touch handle（供 LVGL 使用）
 * @return ESP_OK 成功
 */
esp_err_t bsp_init(esp_lcd_panel_handle_t *out_lcd_panel,
                   esp_lcd_panel_io_handle_t *out_lcd_io,
                   esp_lcd_touch_handle_t *out_touch);

// ============================================================================
// drv_pca9557 - IO 扩展芯片 (I2C @ 0x19)
// ============================================================================

/**
 * @brief 初始化 PCA9557 IO 扩展芯片
 *
 * 配置: BIT0/BIT1/BIT2 设为输出 (CONFIG = 0xF8)
 * 默认: LCD_CS=1, PA_EN=0, DVP_PWDN=1
 */
esp_err_t drv_pca9557_init(void);

/**
 * @brief 设置 PCA9557 某个输出位的电平
 * @param gpio_bit BIT(0)/BIT(1)/BIT(2)
 * @param level 0=低, 1=高
 */
esp_err_t drv_pca9557_set_pin(uint8_t gpio_bit, uint8_t level);

/**
 * @brief 读取 PCA9557 某个引脚的输入值
 * @param gpio_bit BIT(0)/BIT(1)/BIT(2)
 * @param out_level 输出参数，读取的电平值
 */
esp_err_t drv_pca9557_get_pin(uint8_t gpio_bit, uint8_t *out_level);

// PCA9557 引脚定义 (与 main.c 保持一致)
#define DRV_PCA9557_LCD_CS     BIT(0)
#define DRV_PCA9557_PA_EN      BIT(1)
#define DRV_PCA9557_DVP_PWDN   BIT(2)

// ============================================================================
// LCD 面板参数 (与硬件规格一致)
// ============================================================================
#define DRV_LCD_H_RES   320
#define DRV_LCD_V_RES   240

// ============================================================================
// drv_st7789 - LCD 面板驱动 (SPI)
// ============================================================================

/**
 * @brief 初始化 ST7789 LCD 面板
 *
 * 依赖: bsp_init() 已调用 (I2C/SPI 总线已就绪)
 * 内部: spi_bus → panel_io → st7789_device → reset/init
 *
 * @param cs_gpio_by_pca9557 是否由 PCA9557 控制 CS (true=是, false=用 GPIO)
 * @return panel handle 用于后续 HAL 层
 */
esp_err_t drv_st7789_init(bool cs_gpio_by_pca9557);
esp_err_t drv_st7789_get_panel_handle(esp_lcd_panel_handle_t *out_handle);
esp_err_t drv_st7789_get_io_handle(esp_lcd_panel_io_handle_t *out_io_handle);

// ============================================================================
// drv_ft6336 - 触摸芯片驱动 (I2C @ 0x38)
// ============================================================================

/**
 * @brief 初始化 FT6336 触摸芯片
 *
 * 依赖: bsp_init() 已调用
 * 内部: 创建 I2C panel_io → ft5x06 device
 *
 * @return touch handle 用于 HAL 层
 */
esp_err_t drv_ft6336_init(void);
esp_err_t drv_ft6336_get_touch_handle(esp_lcd_touch_handle_t *out_handle);

// ============================================================================
// drv_ledc - LEDC PWM 封装
// ============================================================================

/**
 * @brief 初始化 LCD 背光 LEDC
 *
 * 配置: Channel 0, Timer 0, Low Speed, 5kHz, 10-bit
 * GPIO42 输出，反相 (output_invert=true 因为背光电路是反相的)
 */
esp_err_t drv_ledc_init(void);

/**
 * @brief 设置背光亮度
 * @param percent 0-100
 */
esp_err_t drv_ledc_set_brightness(uint8_t percent);

// ============================================================================
// drv_key - BOOT 按键驱动
// ============================================================================

/**
 * @brief 按键事件类型
 */
typedef enum {
    DRV_KEY_EVT_CLICK,          // 单击
    DRV_KEY_EVT_DOUBLE_CLICK,   // 双击
    DRV_KEY_EVT_LONG_PRESS,     // 长按 (>1.5s)
    DRV_KEY_EVT_VERY_LONG_PRESS, // 极长按 (>3s)
} drv_key_evt_t;

/**
 * @brief 按键事件回调
 * @param evt 事件类型
 * @param user_data 用户参数
 */
typedef void (*drv_key_cb_t)(drv_key_evt_t evt, void *user_data);

/**
 * @brief 初始化 BOOT 按键 (GPIO0)
 *
 * 配置: 下降沿中断，内部上拉
 * 内部创建 task 检测 debounce/双击/长按
 */
esp_err_t drv_key_init(void);

/**
 * @brief 注册按键事件回调
 */
esp_err_t drv_key_register_callback(drv_key_cb_t cb, void *user_data);

// ============================================================================
// drv_qmi8658 - IMU 驱动 (I2C @ 0x6A)
// ============================================================================

/**
 * @brief 初始化 QMI8658 IMU
 *
 * 配置: ACC ±4g 250Hz, GYR ±512dps 250Hz
 */
esp_err_t drv_qmi8658_init(void);

/**
 * @brief 反初始化 QMI8658
 */
esp_err_t drv_qmi8658_deinit(void);

/**
 * @brief 读取 IMU 数据
 *
 * @param out_acc_x/y/z 输出加速度 m/s²
 * @param out_gyr_x/y/z 输出角速度 °/s
 */
esp_err_t drv_qmi8658_read(float *out_acc_x, float *out_acc_y, float *out_acc_z,
                           float *out_gyr_x, float *out_gyr_y, float *out_gyr_z);

#ifdef __cplusplus
}
#endif
