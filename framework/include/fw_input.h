/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input
 *
 * 全局输入路由：BOOT 键（单击返回 / 双击桌面 / 长按电源菜单）与滑动手势（左滑返回、右滑关闭浮层）。
 * 通知中心 / 控制中心 / 返回 / 主页的按钮位于状态栏（fw_statusbar）。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化输入路由（注册 BOOT 键回调 + 订阅手势事件）
 */
esp_err_t fw_input_init(void);

#ifdef __cplusplus
}
#endif
