/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Touch
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸点（FT6336 为单点）
 */
typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
    uint8_t touch_id;   // 触摸 ID（单点固定为 0）
} periph_touch_point_t;

/**
 * @brief 触摸事件类型
 */
typedef enum {
    PERIPH_TOUCH_EVT_PRESS,
    PERIPH_TOUCH_EVT_RELEASE,
    PERIPH_TOUCH_EVT_TAP,
    PERIPH_TOUCH_EVT_DOUBLE_TAP,
    PERIPH_TOUCH_EVT_LONG_PRESS,
    PERIPH_TOUCH_EVT_SWIPE_LEFT,
    PERIPH_TOUCH_EVT_SWIPE_RIGHT,
    PERIPH_TOUCH_EVT_SWIPE_UP,
    PERIPH_TOUCH_EVT_SWIPE_DOWN,
} periph_touch_evt_t;

/**
 * @brief 触摸事件回调
 */
typedef void (*periph_touch_cb_t)(periph_touch_evt_t evt, const periph_touch_point_t *pt, void *user);

/**
 * @brief 初始化触摸
 */
esp_err_t periph_touch_init(void);

/**
 * @brief 注册触摸事件回调（回调在触摸扫描任务中执行，不可阻塞）
 */
esp_err_t periph_touch_register_callback(periph_touch_cb_t cb, void *user);

/**
 * @brief 同步读取最新触摸点
 */
esp_err_t periph_touch_read(periph_touch_point_t *point);

#ifdef __cplusplus
}
#endif
