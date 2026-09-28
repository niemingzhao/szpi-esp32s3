/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Network
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 网络模式
 */
typedef enum {
    SVC_NET_MODE_OFF,
    SVC_NET_MODE_STA,
    SVC_NET_MODE_AP,
    SVC_NET_MODE_STA_AP,
} svc_net_mode_t;

typedef struct {
    char ssid[33];
    char password[64];
} svc_net_wifi_creds_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_mode;
} svc_net_wifi_ap_t;

typedef struct {
    bool wifi_connected;
    char wifi_ssid[33];
    char ip_addr[16];
    int8_t rssi;
} svc_net_status_t;

esp_err_t svc_net_init(void);

/** 启动 Wi-Fi */
esp_err_t svc_net_wifi_start(svc_net_mode_t mode);
esp_err_t svc_net_wifi_stop(void);

/** 扫描 AP */
esp_err_t svc_net_wifi_scan(svc_net_wifi_ap_t *aps, size_t max_aps, size_t *found, uint32_t timeout_ms);

/** 连接（已知 SSID） */
esp_err_t svc_net_wifi_connect(const svc_net_wifi_creds_t *creds);
esp_err_t svc_net_wifi_disconnect(void);

/** 获取已保存的凭据并尝试自动连接 */
esp_err_t svc_net_wifi_auto_connect(void);

/** 清除已保存的凭据并断开 / 关闭 Wi-Fi */
esp_err_t svc_net_wifi_forget(void);

/** 读取已保存的 SSID（无则返回 ESP_ERR_NOT_FOUND） */
esp_err_t svc_net_wifi_get_saved_ssid(char *buf, size_t len);

/** SmartConfig 配网 */
esp_err_t svc_net_smartconfig_start(void);
esp_err_t svc_net_smartconfig_stop(void);

/** 状态查询 */
esp_err_t svc_net_get_status(svc_net_status_t *status);

/** HTTP 客户端 */
esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms);

/** MQTT */
esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password);
esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos);
esp_err_t svc_mqtt_subscribe(const char *topic, int qos, void (*cb)(const char *topic, const char *payload));
esp_err_t svc_mqtt_disconnect(void);

/** WebSocket */
esp_err_t svc_ws_connect(const char *uri, void (*cb)(const char *data, size_t len));
esp_err_t svc_ws_send(const char *data, size_t len);
esp_err_t svc_ws_disconnect(void);

/** OTA */
esp_err_t svc_ota_check_and_update(const char *url);

#ifdef __cplusplus
}
#endif
