/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Hardware Abstraction Layer
 * 聚合各模块头文件，并提供整层初始化入口。
 *
 * 命名：periph_<模块>_<功能>
 */

#pragma once

#include "periph_lcd.h"
#include "periph_touch.h"
#include "periph_audio.h"
#include "periph_imu.h"
#include "periph_storage.h"
#include "periph_io_exp.h"
#include "periph_button.h"
#include "periph_ext.h"         /* 外扩接口（GPIO / PWM / I2C / UART / ADC） */
#include "periph_camera.h"      /* 摄像头按需初始化（不参与 peripherals_init_all） */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化所有外设
 *
 * 调用顺序（依赖 drivers/bsp_init）:
 *   io_exp → audio → lcd(含 LVGL display) → touch → imu → storage → button → ext
 *
 * @return ESP_OK 成功
 */
esp_err_t peripherals_init_all(void);

#ifdef __cplusplus
}
#endif
