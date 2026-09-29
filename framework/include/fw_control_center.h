/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Control Center
 *
 * 控制中心（全局浮层，默认隐藏）：Wi-Fi 磁贴、亮度、音量、试听。
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

/** 换主题后重建全部控件 */
esp_err_t fw_control_center_rebuild(void);

#ifdef __cplusplus
}
#endif
