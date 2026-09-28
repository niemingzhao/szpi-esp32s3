/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Common
 * 聚合各模块头文件，并提供整层初始化入口。
 *
 * 命名：fw_<模块>_<功能>
 */

#pragma once

#include "esp_err.h"
#include "fw_theme.h"
#include "fw_asset.h"
#include "fw_window.h"
#include "fw_app_mgr.h"
#include "fw_statusbar.h"
#include "fw_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 固件版本与当前层（供日志与关于页使用） */
#define SZPI_OS_VERSION   "v0.4"
#define SZPI_OS_LAYER     "Framework Layer"

/**
 * @brief 初始化 Framework 层
 *
 * 顺序：Theme → Asset → Window → AppMgr → StatusBar → Input
 *
 * 依赖：services_init() 已完成，且 LVGL display 已由 periph_lcd_init() 创建。
 *
 * @return ESP_OK 成功
 */
esp_err_t fw_init(void);

#ifdef __cplusplus
}
#endif
