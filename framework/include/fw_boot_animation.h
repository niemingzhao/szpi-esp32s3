/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Boot Animation
 *
 * 启动动画：黑屏 → Logo 缩放淡入 300 ms → 旋转 500 ms → 淡出 300 ms。
 * 只依赖 LVGL 与主题 / 资源，不依赖其他 framework 模块。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 播放启动动画并阻塞至结束（最坏 ~1.1 s）
 */
esp_err_t fw_boot_animation(void);

#ifdef __cplusplus
}
#endif
