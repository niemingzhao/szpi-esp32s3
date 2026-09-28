/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Control Center
 *
 * 下拉控制中心（全局浮层，默认隐藏）：Wi-Fi 开关、亮度、音量。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fw_control_center_init(void);
esp_err_t fw_control_center_show(void);
esp_err_t fw_control_center_hide(void);
esp_err_t fw_control_center_toggle(void);
bool fw_control_center_is_visible(void);

#ifdef __cplusplus
}
#endif
