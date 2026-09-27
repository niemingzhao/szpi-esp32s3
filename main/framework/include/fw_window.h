/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Window
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化窗口管理（记录当前活动屏）
 */
esp_err_t fw_window_init(void);

/**
 * @brief 切换到某个屏（带过渡动画）
 *
 * 调用者需持有 LVGL 锁。动画期间由 LVGL 自行处理清屏，不自动删除旧屏。
 *
 * @param scr      目标屏（App 的 root）
 * @param anim     动画类型（如 LV_SCREEN_LOAD_ANIM_FADE_IN）
 * @param time_ms  动画时长（0 表示无动画）
 */
esp_err_t fw_window_switch_to(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time_ms);

/**
 * @brief 用 LVGL 的实际活动屏重新对齐内部记录（调用者需持有 LVGL 锁）
 *
 * 重建 UI（如切换主题）时，旧的屏会被删除，内部记录的指针会悬空，
 * 且可能被新屏复用同一地址。重建过程中调用本函数可避免误判。
 */
esp_err_t fw_window_sync_active(void);

#ifdef __cplusplus
}
#endif
