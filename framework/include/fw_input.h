/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Input
 *
 * 全局输入路由：
 *   BOOT 键单击   → fw_app_mgr_back()
 *   BOOT 键双击   → fw_app_mgr_back_to_home()
 *   BOOT 键长按   → 电源菜单（后续阶段）
 *   底部虚拟按键栏 BACK / HOME
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

/**
 * @brief 创建全局底部虚拟按键栏（BACK / HOME）
 *
 * 挂在 lv_layer_top() 上，内部自行加 LVGL 锁。
 */
esp_err_t fw_input_create_navbar(void);

#ifdef __cplusplus
}
#endif
