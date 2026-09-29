/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - 锁屏
 *
 * 全屏浮层（盖住状态栏），显示时钟与解锁提示。解锁方式：上滑（手势由 fw_input 路由
 * 过来）或长按锁屏界面。控制中心的“锁定”磁贴调用 fw_lockscreen_lock()。
 * 注：PRD / UI 规范里没有锁屏条目，只有控制中心布局稿里的 Lock 磁贴，行为按上面定义。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建锁屏浮层（默认隐藏）
 */
esp_err_t fw_lockscreen_init(void);

/**
 * @brief 换主题后重建（保留锁定状态）
 */
esp_err_t fw_lockscreen_rebuild(void);

/**
 * @brief 锁定（显示浮层）
 */
esp_err_t fw_lockscreen_lock(void);

/**
 * @brief 解锁（隐藏浮层）
 */
esp_err_t fw_lockscreen_unlock(void);

/**
 * @brief 当前是否已锁定
 */
bool fw_lockscreen_is_locked(void);

#ifdef __cplusplus
}
#endif
