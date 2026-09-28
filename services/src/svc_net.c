/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Network（骨架）
 *
 * 说明：Wi-Fi / BLE / HTTP / MQTT / WebSocket / OTA 尚未接入，
 *       本服务先提供完整接口，功能返回 ESP_ERR_NOT_SUPPORTED。
 */

#include "svc_common.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "svc.net";

static bool s_initialized = false;

esp_err_t svc_net_init(void)
{
    s_initialized = true;
    ESP_LOGI(TAG, "initialized (skeleton)");
    return ESP_OK;
}

esp_err_t svc_net_wifi_start(svc_net_mode_t mode)
{
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_wifi_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_wifi_scan(svc_net_wifi_ap_t *aps, size_t max_aps, size_t *found, uint32_t timeout_ms)
{
    (void)aps;
    (void)max_aps;
    (void)timeout_ms;
    if (found) *found = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_wifi_connect(const svc_net_wifi_creds_t *creds)
{
    (void)creds;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_wifi_disconnect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_wifi_auto_connect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_smartconfig_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_smartconfig_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_net_get_status(svc_net_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    return ESP_OK;
}

esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms)
{
    (void)url;
    (void)resp_buf;
    (void)buf_len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms)
{
    (void)url;
    (void)body;
    (void)resp_buf;
    (void)buf_len;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password)
{
    (void)uri;
    (void)username;
    (void)password;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos)
{
    (void)topic;
    (void)payload;
    (void)qos;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_mqtt_subscribe(const char *topic, int qos, void (*cb)(const char *topic, const char *payload))
{
    (void)topic;
    (void)qos;
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_mqtt_disconnect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_ws_connect(const char *uri, void (*cb)(const char *data, size_t len))
{
    (void)uri;
    (void)cb;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_ws_send(const char *data, size_t len)
{
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_ws_disconnect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_ota_check_and_update(const char *url)
{
    (void)url;
    return ESP_ERR_NOT_SUPPORTED;
}
