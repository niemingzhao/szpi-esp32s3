/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 蓝牙 BLE HID 设备
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 键盘 / 消费类用法值（完整表见 HID 规范；服务内部表见 svc_bt_hid_report.h） */
#define SVC_BT_HID_KEY_ENTER           0x28
#define SVC_BT_HID_KEY_ESCAPE          0x29
#define SVC_BT_HID_KEY_BACKSPACE       0x2A
#define SVC_BT_HID_KEY_TAB             0x2B
#define SVC_BT_HID_KEY_SPACE           0x2C
#define SVC_BT_HID_KEY_RIGHT_ARROW     0x4F
#define SVC_BT_HID_KEY_LEFT_ARROW      0x50
#define SVC_BT_HID_KEY_DOWN_ARROW      0x51
#define SVC_BT_HID_KEY_UP_ARROW        0x52
#define SVC_BT_HID_CONSUMER_PLAY       0xB0
#define SVC_BT_HID_CONSUMER_PAUSE      0xB1
#define SVC_BT_HID_CONSUMER_MUTE       0xE2
#define SVC_BT_HID_CONSUMER_VOL_UP     0xE9
#define SVC_BT_HID_CONSUMER_VOL_DOWN   0xEA

/* 鼠标报告的按键位（svc_bt_hid_mouse / svc_bt_hid_mouse_click 的 buttons） */
#define SVC_BT_HID_MOUSE_LEFT          0x01
#define SVC_BT_HID_MOUSE_RIGHT         0x02
#define SVC_BT_HID_MOUSE_MIDDLE        0x04

/**
 * @brief 启动 BLE HID 设备（需 svc_bt_init() 已完成）
 *
 * 设备作为标准 BLE HID 从机（键盘 / 鼠标 / 消费类控制）广播，主机（手机 /
 * 电脑）配对后即可用下面的接口发送按键与鼠标动作。
 */
esp_err_t svc_bt_hid_init(void);

/**
 * @brief 是否已连接且配对完成（此时才能发送报告）
 */
bool svc_bt_hid_is_ready(void);

/**
 * @brief 发送键盘报告
 *
 * @param[in] usage     HID 用法值（0x04 = a、0x28 = 回车、0x00 = 松开所有键）
 * @param[in] modifier  修饰键掩码（bit0 左 Ctrl … bit7 右 GUI），无修饰传 0
 */
esp_err_t svc_bt_hid_key(uint8_t usage, uint8_t modifier);

/**
 * @brief 发送鼠标报告
 *
 * @param[in] buttons  按键位，SVC_BT_HID_MOUSE_LEFT / RIGHT / MIDDLE 的按位或
 * @param[in] dx, dy   相对位移（-127 ~ 127）
 */
esp_err_t svc_bt_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy);

/**
 * @brief 发送消费类控制（音量 / 播放等）
 *
 * @param[in] usage    HID 消费类用法值（0xE9 = 音量加、0xEA = 音量减）
 * @param[in] pressed  true = 按下，false = 松开
 */
esp_err_t svc_bt_hid_consumer(uint8_t usage, bool pressed);

/**
 * @brief 单击一个键盘键（自动在 30 ms 后松开）
 */
esp_err_t svc_bt_hid_key_click(uint8_t usage, uint8_t modifier);

/**
 * @brief 单击一个消费类键（自动在 30 ms 后松开）
 */
esp_err_t svc_bt_hid_consumer_click(uint8_t usage);

/**
 * @brief 单击一个鼠标键（自动在 30 ms 后松开）
 *
 * @param[in] buttons  SVC_BT_HID_MOUSE_LEFT / RIGHT / MIDDLE
 */
esp_err_t svc_bt_hid_mouse_click(uint8_t buttons);

#ifdef __cplusplus
}
#endif
