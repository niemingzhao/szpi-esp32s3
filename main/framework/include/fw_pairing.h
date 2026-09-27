/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Pairing（蓝牙配对提示）
 *
 * 配对参数是"安全连接 + MITM + 绑定"，所以协议栈配对时会找用户确认：要么两端显示
 * 同一串 6 位码问是否一致，要么对端显示 6 位码等我们输进去。
 *
 * 提示做成全局的（挂在 lv_layer_top()），不做在蓝牙 App 里 —— 广播在离开 App 之后
 * 仍然有效，手机 / 电脑可能在 App 关着的时候来配对。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化配对提示（订阅 SVC_EVENT_BT_PAIR_PROMPT）
 */
esp_err_t fw_pairing_init(void);

#ifdef __cplusplus
}
#endif
