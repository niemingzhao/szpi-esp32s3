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

/**
 * @brief 创建状态栏并订阅系统事件
 *
 * 内部自行加 LVGL 锁。
 */
esp_err_t fw_statusbar_init(void);

/**
 * @brief 设置时间文本（如 "10:30"）
 */
esp_err_t fw_statusbar_set_time(const char *time);

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

#ifdef __cplusplus
}
#endif
