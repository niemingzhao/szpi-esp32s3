/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Common
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册所有内置 App（实现见 apps/src/app_register.c）
 *
 * 依赖 fw_app_mgr_init() 已完成。
 */
void app_register_all(void);

#ifdef __cplusplus
}
#endif
