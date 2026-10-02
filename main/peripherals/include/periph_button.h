/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Button (BOOT key)
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按键事件类型
 *
 * 长按在按住期间达到阈值即上报，不需要等松手。
 */
typedef enum {
    PERIPH_BTN_EVT_CLICK,
    PERIPH_BTN_EVT_DOUBLE_CLICK,
    PERIPH_BTN_EVT_LONG_PRESS,
} periph_button_evt_t;

/**
 * @brief 按键事件回调
 */
typedef void (*periph_button_cb_t)(periph_button_evt_t evt, void *user);

/**
 * @brief 初始化按键（BOOT key）
 */
esp_err_t periph_button_init(void);

/**
 * @brief 注册按键事件回调
 */
esp_err_t periph_button_register_callback(periph_button_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
