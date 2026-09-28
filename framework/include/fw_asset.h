/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Asset
 *
 * 统一资源入口：字体与图标。
 * Scope A 只提供 LVGL 内置字体与内置符号图标；中文子集字体与 64x64
 * PNG 图标资源在后续阶段通过本模块加载，调用方不改。
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化资源模块（Scope A 无外部资源，仅占位）
 */
esp_err_t fw_asset_init(void);

/**
 * @brief 字体：小号（14 px，拉丁 / 数字），不可用时回退 LVGL 默认字体
 */
const lv_font_t *fw_asset_font_14(void);

/**
 * @brief 字体：中号（20 px），不可用时回退 LVGL 默认字体
 */
const lv_font_t *fw_asset_font_20(void);

/**
 * @brief 字体：常规（24 px）
 */
const lv_font_t *fw_asset_font_24(void);

/**
 * @brief 字体：大号（32 px），未启用时回退 24 px
 */
const lv_font_t *fw_asset_font_32(void);

/**
 * @brief 字体：中文（14 px，正文 / 标签；同样覆盖 ASCII）
 */
const lv_font_t *fw_asset_font_cn(void);

/**
 * @brief 字体：中文大号（16 px，标题 / 强调）
 */
const lv_font_t *fw_asset_font_cn_large(void);

/**
 * @brief 按 App 名取内置符号图标（无匹配时返回通用文件图标）
 */
const char *fw_asset_symbol_for(const char *app_name);

#ifdef __cplusplus
}
#endif
