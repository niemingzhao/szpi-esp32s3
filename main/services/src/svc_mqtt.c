/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - MQTT 实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include <string.h>

static const char *TAG = "svc.mqtt";

/* 主题 / 载荷按小消息设计：更长的主题截断，更长的载荷截断，分片大消息丢弃 */
#define SVC_MQTT_TOPIC_MAX    80
#define SVC_MQTT_PAYLOAD_MAX  256

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static svc_mqtt_msg_cb_t s_cb = NULL;
static void *s_cb_user = NULL;
static char s_sub_topic[SVC_MQTT_TOPIC_MAX] = { 0 };
static int s_sub_qos = 0;

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args;
    (void)base;

    const esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to broker");
        if (s_sub_topic[0] != '\0') {
            esp_mqtt_client_subscribe(s_client, s_sub_topic, s_sub_qos);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        break;

    case MQTT_EVENT_DATA: {
        /* esp-mqtt 会把超过接收缓冲的大消息拆成多次事件（data_len < total_data_len）：
         * 本服务按小消息设计，遇到分片直接丢，免得把半截数据当成一条消息上报 */
        if (e->total_data_len != e->data_len) {
            ESP_LOGW(TAG, "drop chunked msg (%d/%d)", e->data_len, e->total_data_len);
            break;
        }

        /* 事件里的 topic / data 不保证以 '\0' 结尾，先做临时拷贝 */
        char topic[SVC_MQTT_TOPIC_MAX] = { 0 };
        char payload[SVC_MQTT_PAYLOAD_MAX] = { 0 };
        int tl = (e->topic_len < (int)sizeof(topic) - 1) ? e->topic_len : (int)sizeof(topic) - 1;
        int dl = (e->data_len < (int)sizeof(payload) - 1) ? e->data_len : (int)sizeof(payload) - 1;

        if (tl > 0 && e->topic != NULL) memcpy(topic, e->topic, (size_t)tl);
        if (dl > 0 && e->data != NULL) memcpy(payload, e->data, (size_t)dl);

        ESP_LOGI(TAG, "msg %s: %s", topic, payload);
        if (s_cb != NULL) {
            s_cb(topic, payload, (size_t)dl, s_cb_user);
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        break;

    default:
        break;
    }
}

esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password)
{
    if (uri == NULL || uri[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_client != NULL) return ESP_ERR_INVALID_STATE;

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,   /* mqtts 走内置证书 bundle */
        .credentials = {
            .username = username,
            .authentication = {
                .password = password,
            },
        },
        .session.keepalive = 30,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "connecting to %s", uri);
    return ESP_OK;
}

esp_err_t svc_mqtt_disconnect(void)
{
    if (s_client == NULL) return ESP_ERR_INVALID_STATE;

    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);

    s_client = NULL;
    s_connected = false;
    s_cb = NULL;
    s_cb_user = NULL;
    s_sub_topic[0] = '\0';
    s_sub_qos = 0;

    ESP_LOGI(TAG, "disconnected");
    return ESP_OK;
}

bool svc_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos)
{
    if (s_client == NULL || !s_connected) return ESP_ERR_INVALID_STATE;
    if (topic == NULL || payload == NULL) return ESP_ERR_INVALID_ARG;
    if (qos < 0 || qos > 2) return ESP_ERR_INVALID_ARG;

    int id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, 0);
    return (id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_mqtt_subscribe(const char *topic, int qos, svc_mqtt_msg_cb_t cb, void *user)
{
    if (s_client == NULL) return ESP_ERR_INVALID_STATE;
    if (topic == NULL || topic[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (qos < 0 || qos > 2) return ESP_ERR_INVALID_ARG;

    /* 只有一个订阅槽：换主题时先把 broker 侧上一个退掉。
     * 只覆盖本地记录是不够的 —— 旧主题仍处于订阅状态，它来的消息会投递到新回调上 */
    if (s_connected && s_sub_topic[0] != '\0' && strcmp(s_sub_topic, topic) != 0) {
        esp_mqtt_client_unsubscribe(s_client, s_sub_topic);
    }

    strlcpy(s_sub_topic, topic, sizeof(s_sub_topic));
    s_sub_qos = qos;
    s_cb = cb;
    s_cb_user = user;

    if (!s_connected) return ESP_OK;      /* 连上后由事件回调补订阅 */

    int id = esp_mqtt_client_subscribe(s_client, topic, qos);
    return (id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_mqtt_unsubscribe(const char *topic)
{
    if (s_client == NULL || topic == NULL) return ESP_ERR_INVALID_ARG;

    /* 只有一个订阅槽：退掉的正好是记住的主题时，连回调一起清掉 */
    if (strcmp(s_sub_topic, topic) == 0) {
        s_sub_topic[0] = '\0';
        s_sub_qos = 0;
        s_cb = NULL;
        s_cb_user = NULL;
    }

    if (!s_connected) return ESP_OK;
    return esp_mqtt_client_unsubscribe(s_client, topic);
}
