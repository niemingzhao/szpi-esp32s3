/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - WebSocket 客户端（基于 espressif/esp_websocket_client）
 *
 * 断线自动重连由组件负责（reconnect_timeout_ms）；wss:// 用 IDF 证书 bundle 校验。
 * 注意：svc_ws_disconnect() 不能在收到消息的回调里调用（组件禁止在事件回调里
 * stop/destroy），需要断开时请从其他任务调用。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 收到消息的回调（data 只在回调期间有效；可能是分片的一部分） */
typedef void (*svc_ws_msg_cb_t)(const char *data, size_t len, void *user);

/**
 * @brief 连接 WebSocket（ws:// 或 wss://）
 */
esp_err_t svc_ws_connect(const char *uri, svc_ws_msg_cb_t cb, void *user);

/**
 * @brief 发送文本消息（不能在消息回调里调用）
 */
esp_err_t svc_ws_send(const char *data, size_t len);

/**
 * @brief 断开并销毁客户端（不能在消息回调里调用）
 */
esp_err_t svc_ws_disconnect(void);

/**
 * @brief 是否已连接
 */
bool svc_ws_is_connected(void);

#ifdef __cplusplus
}
#endif
