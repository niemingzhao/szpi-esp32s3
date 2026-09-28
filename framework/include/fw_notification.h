/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Notification Center
 *
 * 通知中心（全局浮层，默认隐藏）+ 新通知 Toast。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fw_notification_init(void);
esp_err_t fw_notification_show(void);
esp_err_t fw_notification_hide(void);
esp_err_t fw_notification_toggle(void);
esp_err_t fw_notification_clear_all(void);
bool fw_notification_is_visible(void);

/** 换主题后重建全部控件 */
esp_err_t fw_notification_rebuild(void);

#ifdef __cplusplus
}
#endif
