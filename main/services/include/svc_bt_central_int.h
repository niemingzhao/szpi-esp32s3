/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - svc_bt 与 svc_bt_central 之间的内部接口
 */

#pragma once

#include "esp_err.h"
#include "esp_gattc_api.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bluedroid 只允许一个 GATTC 回调，由 svc_bt.c 统一注册，再把事件转发到这里 */
void svc_bt_central_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param);

/* 注册中心角色的 GATTC app（由 svc_bt_init 调用） */
esp_err_t svc_bt_central_init(void);

/* 这条链路是否由中心角色主动发起（对端连进来的链路返回 false）。
 * esp_ble_gattc_open() 会给每个 GATTS app 也发一次 CONNECT / DISCONNECT，从机侧
 * （含 HID）用它把中心链路过滤掉 */
bool svc_bt_central_owns_link(const uint8_t bda[6]);

#ifdef __cplusplus
}
#endif
