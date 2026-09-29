/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Status Bar
 *
 * 全局状态栏，挂在 lv_layer_top() 上，不随屏幕切换消失。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态栏高度（App 内容区与浮层都以此为顶部偏移） */
#define FW_STATUSBAR_H   28

/**
 * @brief 创建状态栏并订阅系统事件
 *
 * 状态栏为全局浮层，内含：返回 / 主页 / 通知中心 / 控制中心 按钮、时间、状态图标。
 * 内部自行加 LVGL 锁。
 */
esp_err_t fw_statusbar_init(void);

/** 换主题后重建全部控件（保留订阅与定时器） */
esp_err_t fw_statusbar_rebuild(void);

/**
 * @brief 设置 Wi-Fi 图标
 */
esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected);

/**
 * @brief 设置音乐播放图标
 */
esp_err_t fw_statusbar_set_music_playing(bool on);

/**
 * @brief 设置蓝牙图标
 */
esp_err_t fw_statusbar_set_bluetooth(bool on);

/**
 * @brief 设置亮度图标（背光 0 时灰、<50% 次级色、>=50% 主色）
 */
esp_err_t fw_statusbar_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
