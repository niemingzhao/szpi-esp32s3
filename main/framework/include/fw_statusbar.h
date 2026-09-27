/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Status Bar
 *
 * 全局状态栏，挂在 lv_layer_top() 上，不随屏幕切换消失。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 状态栏高度（App 内容区与浮层都以此为顶部偏移） */
#define FW_STATUSBAR_H   28

/**
 * @brief 创建状态栏并订阅系统事件
 *
 * 状态栏为全局浮层，内含：返回 / 主页按钮、时间、状态图标
 * （Wi-Fi / 蓝牙 / 声音 / 录音 / 摄像头 / TF 卡，常显，启用时高亮）。
 * 图标由各服务的事件驱动，另有 1 s 轮询兜底。内部自行加 LVGL 锁。
 */
esp_err_t fw_statusbar_init(void);

/** 换主题后重建全部控件（保留订阅与定时器） */
esp_err_t fw_statusbar_rebuild(void);

#ifdef __cplusplus
}
#endif
