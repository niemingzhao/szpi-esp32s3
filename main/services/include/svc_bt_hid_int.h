/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - svc_bt 与 svc_bt_hid 之间的内部接口
 */

#pragma once

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* svc_bt.c 把 GAP / GATTS 事件转发给 HID（HID 只处理属于自己 app 的事件） */
void svc_bt_hid_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
void svc_bt_hid_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                            esp_ble_gatts_cb_param_t *param);

/* svc_bt_hid.c 收到连接 / 断开时同步 svc_bt 的连接状态（HID 与自定义服务共用一条链路）。
 * 断开时 bda 传 NULL */
void svc_bt_note_conn(bool connected, uint16_t conn_id, const uint8_t bda[6]);

#ifdef __cplusplus
}
#endif
