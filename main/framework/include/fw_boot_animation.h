/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Boot Animation
 *
 * 开机画面：全屏黑底 + 立创官方 Logo 静态展示，同时播一声提示音。
 * 依赖 LVGL、fw_logo 的开机 Logo 与 svc_audio 的提示音接口。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放开机画面并阻塞至结束（约 2.05 s，见实现里的时长常量）
 */
esp_err_t fw_boot_animation(void);

#ifdef __cplusplus
}
#endif
