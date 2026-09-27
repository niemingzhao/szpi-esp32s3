/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 触摸（FT6336，单点）
 *
 * FT6336 为单点触摸，只提供按下 / 抬起 / 点击 / 长按，不做多指与滑动手势。
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
} periph_touch_point_t;

/**
 * @brief 触摸事件类型
 */
typedef enum {
    PERIPH_TOUCH_EVT_PRESS,       // 按下
    PERIPH_TOUCH_EVT_RELEASE,     // 抬起
    PERIPH_TOUCH_EVT_TAP,         // 点击
    PERIPH_TOUCH_EVT_LONG_PRESS,  // 长按
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
 * @brief 丢弃下一次触摸（含当前这次，直到抬手）
 *
 * 熄屏后由 svc_power 调用：这一整次触摸只用于唤醒，不写进 LVGL 的输入缓存，
 * 事件回调照常触发。抬手后自动恢复正常。
 */
esp_err_t periph_touch_drop_next_gesture(void);

#ifdef __cplusplus
}
#endif
