/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - WebSocket 实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "svc.ws";

static esp_websocket_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static svc_ws_msg_cb_t s_cb = NULL;
static void *s_cb_user = NULL;

static void ws_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args;
    (void)base;

    const esp_websocket_event_data_t *e = (const esp_websocket_event_data_t *)data;

    switch ((esp_websocket_event_id_t)id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;

    case WEBSOCKET_EVENT_DATA:
        /* 只收完整的一帧文本：
         *   - 超过组件缓冲的大消息会按 payload_offset 分多次事件送达
         *   - 协议层分片的帧 fin=false（后面跟 continuation 帧）
         *   - 二进制帧、PING / PONG / CLOSE 等控制帧都不是文本消息
         * 以上都直接丢，免得把半截数据或控制帧当成一条消息交给上层 */
        if (e->op_code != 0x01 || !e->fin || e->payload_offset != 0 || e->data_len != e->payload_len) {
            ESP_LOGW(TAG, "drop non-text/fragmented frame (op=0x%02x fin=%d, %d/%d bytes @%d)",
                     e->op_code, (int)e->fin, e->data_len, e->payload_len, e->payload_offset);
            break;
        }

        if (s_cb != NULL) {
            s_cb(e->data_ptr, (size_t)e->data_len, s_cb_user);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "error");
        break;

    default:
        break;
    }
}

esp_err_t svc_ws_connect(const char *uri, svc_ws_msg_cb_t cb, void *user)
{
    if (uri == NULL || uri[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_client != NULL) return ESP_ERR_INVALID_STATE;

    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* wss:// 用 IDF 证书 bundle */
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 1024,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }

    s_cb = cb;
    s_cb_user = user;

    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                 ws_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_websocket_client_start(s_client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_cb = NULL;
        s_cb_user = NULL;
        return err;
    }

    ESP_LOGI(TAG, "connecting to %s", uri);
    return ESP_OK;
}

esp_err_t svc_ws_send(const char *data, size_t len)
{
    if (s_client == NULL || !s_connected) return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    int sent = esp_websocket_client_send_text(s_client, data, (int)len, pdMS_TO_TICKS(2000));
    return (sent == (int)len) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_ws_disconnect(void)
{
    if (s_client == NULL) return ESP_ERR_INVALID_STATE;

    esp_websocket_client_stop(s_client);
    esp_websocket_client_destroy(s_client);

    s_client = NULL;
    s_connected = false;
    s_cb = NULL;
    s_cb_user = NULL;

    ESP_LOGI(TAG, "disconnected");
    return ESP_OK;
}

bool svc_ws_is_connected(void)
{
    return s_connected;
}
