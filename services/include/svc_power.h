/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Power
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief 唤醒 / 休眠（熄屏）
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
