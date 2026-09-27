/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 电源（背光超时 / 休眠 / 亮度）
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触摸点（SVC_EVENT_TOUCH 的负载）
 *
 * Services 对外类型与 Peripherals 的原始类型各一套，避免头文件互相暴露。
 */
typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} svc_touch_point_t;

/**
 * @brief 初始化电源服务（背光超时、休眠）
 */
esp_err_t svc_power_init(void);

/**
 * @brief 设置背光超时秒数（0 = 不超时）
 */
esp_err_t svc_power_set_backlight_timeout(uint32_t seconds);
uint32_t svc_power_get_backlight_timeout(void);

/**
 * @brief 设置 / 获取屏幕亮度（0-100，自动持久化，发布 SVC_EVENT_BRIGHTNESS_CHANGED）
 *
 * 低于最小可见亮度会被抬到该值（0 会让背光全灭、界面看不见）；要熄屏请用
 * svc_power_sleep()，不要用亮度 0。
 */
esp_err_t svc_power_set_brightness(uint8_t percent);
uint8_t svc_power_get_brightness(void);

/**
 * @brief 唤醒 / 休眠（熄屏）
 *
 * wake 会重置屏幕超时的活动计时，可当作"用户有操作"调用；sleep 只关背光、不动 LCD 内容，
 * 并让熄屏后的第一次触摸只用于唤醒（不落到界面）。
 */
esp_err_t svc_power_wake(void);
esp_err_t svc_power_sleep(void);
bool svc_power_is_sleeping(void);

/**
 * @brief 重启 / 关机
 */
esp_err_t svc_power_request_reboot(void);
esp_err_t svc_power_request_shutdown(void);

#ifdef __cplusplus
}
#endif
