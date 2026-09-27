/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - MQTT 客户端（基于 mqtt 组件）
 *
 * 连接是异步的：svc_mqtt_connect() 返回后要等 MQTT_EVENT_CONNECTED 才算连上，
 * 用 svc_mqtt_is_connected() 查询；订阅的消息通过注册的回调上报。
 *
 * 只有一个订阅槽：同一时刻只能有一个主题 + 一个回调，重复订阅会顶掉上一个（断线
 * 重连后的自动补订阅也只补记住的那个主题）。按小消息设计：主题 ≤ 79 字节、载荷 ≤ 255
 * 字节，更长的截断；被 mqtt 客户端拆成多片的大消息直接丢弃。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 收到订阅主题消息的回调（topic / payload 只在回调期间有效） */
typedef void (*svc_mqtt_msg_cb_t)(const char *topic, const char *payload, size_t len, void *user);

/**
 * @brief 连接 broker（uri 形如 mqtt://host:1883 或 mqtts://host:8883）
 *
 * username / password 可为 NULL；已经连接时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password);

/**
 * @brief 断开并销毁客户端（同时清掉记住的订阅与回调）
 */
esp_err_t svc_mqtt_disconnect(void);

/**
 * @brief 是否已连上 broker
 */
bool svc_mqtt_is_connected(void);

/**
 * @brief 发布消息（qos 0/1/2；未连接返回 ESP_ERR_INVALID_STATE）
 *
 * payload 按 C 字符串发送（长度取 strlen，不支持含 NUL 的二进制）。
 */
esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos);

/**
 * @brief 订阅主题（qos 0/1/2）；未连接时会记住主题与 QoS，连上后自动补订阅
 */
esp_err_t svc_mqtt_subscribe(const char *topic, int qos, svc_mqtt_msg_cb_t cb, void *user);

/**
 * @brief 取消订阅（退掉的正好是记住的主题时，连回调一起清掉）
 */
esp_err_t svc_mqtt_unsubscribe(const char *topic);

#ifdef __cplusplus
}
#endif
