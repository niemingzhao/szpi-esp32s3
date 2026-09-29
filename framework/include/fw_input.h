/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input
 *
 * 全局输入路由：BOOT 键（单击返回 / 双击桌面 / 长按电源菜单）。
 * 不使用滑动手势：导航与浮层开关全部由状态栏按钮（fw_statusbar）完成。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化输入路由（注册 BOOT 键回调）
 */
esp_err_t fw_input_init(void);

#ifdef __cplusplus
}
#endif
