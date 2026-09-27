/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - 公共头
 * 聚合各模块头文件，并提供整层初始化入口。
 *
 * 命名：fw_<模块>_<功能>
 */

#pragma once

#include "esp_err.h"
#include "svc_identity.h"
#include "fw_theme.h"
#include "fw_asset.h"
#include "fw_window.h"
#include "fw_app_mgr.h"
#include "fw_ui.h"
#include "fw_script.h"
#include "fw_pairing.h"
#include "fw_statusbar.h"
#include "fw_input.h"
#include "fw_boot_animation.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 Framework 层
 *
 * 顺序：Theme → Asset → Window → AppMgr → UI → Script → StatusBar → Pairing → Input
 *（末尾订阅 SVC_EVENT_THEME_CHANGED，换主题时重建状态栏与当前前台 App 界面）
 *
 * 依赖：services_init() 已完成，且 LVGL display 已由 periph_lcd_init() 创建。
 *
 * @return ESP_OK 成功
 */
esp_err_t fw_init(void);

#ifdef __cplusplus
}
#endif
