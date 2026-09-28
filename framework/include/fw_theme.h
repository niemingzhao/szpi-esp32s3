/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Theme
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 主题
 */
typedef enum {
    FW_THEME_DARK = 0,
    FW_THEME_LIGHT,
} fw_theme_t;

/**
 * @brief 初始化主题（从 NVS 读取，默认深色）
 */
esp_err_t fw_theme_init(void);

/**
 * @brief 切换主题（持久化并发布 SVC_EVENT_THEME_CHANGED）
 */
esp_err_t fw_theme_apply(fw_theme_t theme);

/**
 * @brief 当前主题
 */
fw_theme_t fw_theme_current(void);

/* 调色板（默认深色主题取值见 docs/03-design/02-ui-system.md） */
lv_color_t fw_theme_color_bg_primary(void);
lv_color_t fw_theme_color_bg_secondary(void);
lv_color_t fw_theme_color_bg_card(void);
lv_color_t fw_theme_color_text_primary(void);
lv_color_t fw_theme_color_text_secondary(void);
lv_color_t fw_theme_color_text_disabled(void);
lv_color_t fw_theme_color_accent(void);
lv_color_t fw_theme_color_accent2(void);
lv_color_t fw_theme_color_success(void);
lv_color_t fw_theme_color_warning(void);
lv_color_t fw_theme_color_error(void);
lv_color_t fw_theme_color_divider(void);
lv_color_t fw_theme_color_border(void);

#ifdef __cplusplus
}
#endif
