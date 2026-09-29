/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Network 实现
 *
 * 已支持：Wi-Fi STA（启动 / 停止 / 扫描 / 连接 / 断开 / 自动重连 / 状态）、
 *         凭据持久化（NVS 命名空间 wifi）、事件发布、连接成功后触发 SNTP、
 *         HTTP GET / POST。
 * 未支持：SmartConfig、MQTT、WebSocket、OTA。
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "svc.net";

#define NET_NS              "wifi"
#define NET_SCAN_POLL_MS    100
#define NET_MAX_RETRY       5

static esp_netif_t *s_netif = NULL;
static bool s_wifi_inited = false;
static bool s_started = false;
static wifi_mode_t s_mode = WIFI_MODE_NULL;
static volatile bool s_connecting = false;
static volatile bool s_connect_pending = false;
static volatile bool s_user_disconnect = false;
static volatile bool s_scan_done = false;
static volatile uint8_t s_retry_count = 0;
static svc_net_status_t s_status;
static SemaphoreHandle_t s_status_mux = NULL;   /* s_status 由 Wi-Fi 事件任务写、被任意任务读 */

static void status_lock(void)
{
    if (s_status_mux != NULL) xSemaphoreTake(s_status_mux, portMAX_DELAY);
}

static void status_unlock(void)
{
    if (s_status_mux != NULL) xSemaphoreGive(s_status_mux);
}

static const char *reason_str(int reason)
{
    switch (reason) {
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 210: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case 212: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:  return "OTHER";
    }
}

static void handle_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);
static void handle_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data);

static void handle_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_SCAN_DONE:
        s_scan_done = true;
        svc_event_bus_publish(SVC_EVENT_WIFI_SCAN_DONE, NULL, 0);
        break;

    case WIFI_EVENT_STA_START:
        if (s_connect_pending) {
            s_connect_pending = false;
            esp_wifi_connect();   /* start 完成前发出的连接请求，在此补上 */
        }
        break;

    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "connected to AP");
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        int reason = (d != NULL) ? d->reason : -1;
        ESP_LOGW(TAG, "disconnected (reason=%d %s)", reason, reason_str(reason));

        bool was_connecting = s_connecting;
        s_connecting = false;

        status_lock();
        s_status.wifi_connected = false;
        s_status.rssi = 0;
        memset(s_status.wifi_ssid, 0, sizeof(s_status.wifi_ssid));
        memset(s_status.ip_addr, 0, sizeof(s_status.ip_addr));
        status_unlock();

        if (was_connecting) {
            svc_event_bus_publish(SVC_EVENT_WIFI_CONNECT_FAILED, NULL, 0);
        }
        svc_event_bus_publish(SVC_EVENT_WIFI_DISCONNECTED, NULL, 0);

        if (!s_user_disconnect) {
            if (s_retry_count < NET_MAX_RETRY) {
                s_retry_count++;
                ESP_LOGI(TAG, "reconnect %u/%u", (unsigned)s_retry_count, (unsigned)NET_MAX_RETRY);
                esp_wifi_connect();
            } else {
                s_user_disconnect = true;   /* 停止自动重连，等应用显式重连 */
                ESP_LOGW(TAG, "give up after %u attempts", (unsigned)NET_MAX_RETRY);
                svc_event_bus_publish(SVC_EVENT_WIFI_CONNECT_FAILED, NULL, 0);
            }
        }
        break;
    }

    default:
        break;
    }
}

static void handle_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) return;

    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    char ip[sizeof(s_status.ip_addr)];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));

    wifi_ap_record_t ap;
    bool have_ap = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    status_lock();
    strlcpy(s_status.ip_addr, ip, sizeof(s_status.ip_addr));
    if (have_ap) {
        strlcpy(s_status.wifi_ssid, (const char *)ap.ssid, sizeof(s_status.wifi_ssid));
        s_status.rssi = ap.rssi;
    }
    s_status.wifi_connected = true;
    status_unlock();

    s_connecting = false;
    s_connect_pending = false;
    s_retry_count = 0;

    ESP_LOGI(TAG, "got ip: %s", ip);
    svc_event_bus_publish(SVC_EVENT_WIFI_CONNECTED, NULL, 0);
    svc_time_sync_ntp();   /* 连上就同步时间（svc_time 侧也会订阅，双保险） */
}

esp_err_t svc_net_init(void)
{
    if (s_wifi_inited) return ESP_OK;

    if (s_status_mux == NULL) {
        s_status_mux = xSemaphoreCreateMutex();
        if (s_status_mux == NULL) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* 凭据由 svc_settings 持久化 */
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handle_wifi_event, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handle_ip_event, NULL, NULL);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    memset(&s_status, 0, sizeof(s_status));
    s_wifi_inited = true;
    ESP_LOGI(TAG, "initialized");

    svc_net_wifi_auto_connect();
    return ESP_OK;
}

esp_err_t svc_net_wifi_start(svc_net_mode_t mode)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;
    if (mode == SVC_NET_MODE_OFF) return svc_net_wifi_stop();

    /* 只实现了 STA：AP / STA_AP 没有配置热点参数，不能假装支持 */
    if (mode == SVC_NET_MODE_AP || mode == SVC_NET_MODE_STA_AP) {
        ESP_LOGW(TAG, "AP mode not supported yet");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const wifi_mode_t m = WIFI_MODE_STA;

    /* 已经在同一模式下运行就不重复 start（避免刷日志与多余的 IDF 调用） */
    if (s_started && s_mode == m) return ESP_OK;

    s_user_disconnect = false;

    esp_err_t err = esp_wifi_set_mode(m);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err == ESP_OK) {
        s_started = true;
        s_mode = m;
    }

    ESP_LOGI(TAG, "wifi start (mode=%d): %s", mode, esp_err_to_name(err));
    return err;
}

esp_err_t svc_net_wifi_stop(void)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;

    s_user_disconnect = true;
    s_started = false;
    s_mode = WIFI_MODE_NULL;
    s_connecting = false;

    status_lock();
    s_status.wifi_connected = false;
    status_unlock();

    return esp_wifi_stop();
}

esp_err_t svc_net_wifi_scan(svc_net_wifi_ap_t *aps, size_t max_aps, size_t *found, uint32_t timeout_ms)
{
    if (found != NULL) *found = 0;
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;
    if (!s_started) return ESP_ERR_INVALID_STATE;

    s_scan_done = false;
    svc_event_bus_publish(SVC_EVENT_WIFI_SCAN_STARTED, NULL, 0);

    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t waited = 0;
    while (!s_scan_done && (timeout_ms == 0 || waited < timeout_ms)) {
        vTaskDelay(pdMS_TO_TICKS(NET_SCAN_POLL_MS));
        waited += NET_SCAN_POLL_MS;
    }

    if (!s_scan_done) {
        esp_wifi_scan_stop();
        return ESP_ERR_TIMEOUT;
    }

    /* esp_wifi_scan_get_ap_records() 要求有效缓冲区，即使只是想释放扫描结果 */
    uint16_t num = (aps != NULL && max_aps > 0) ? (uint16_t)max_aps : 1;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs == NULL) return ESP_ERR_NO_MEM;

    err = esp_wifi_scan_get_ap_records(&num, recs);
    if (err == ESP_OK) {
        if (aps != NULL && max_aps > 0) {
            for (uint16_t i = 0; i < num; i++) {
                strlcpy(aps[i].ssid, (const char *)recs[i].ssid, sizeof(aps[i].ssid));
                aps[i].rssi = recs[i].rssi;
                aps[i].auth_mode = (uint8_t)recs[i].authmode;
            }
        }
        if (found != NULL) *found = num;
    }

    free(recs);
    return err;
}

esp_err_t svc_net_wifi_connect(const svc_net_wifi_creds_t *creds)
{
    if (creds == NULL || creds->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;

    /* 已经连在这个网络上就不要再连一次（否则 IDF 会先断开再重连并告警） */
    if (s_status.wifi_connected && strcmp(s_status.wifi_ssid, creds->ssid) == 0) {
        ESP_LOGI(TAG, "already connected to %s", creds->ssid);
        return ESP_OK;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.sta.ssid, creds->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, creds->password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;        /* 兼容 WPA3 / PMF（required=false 保持兼容 WPA2） */
    wc.sta.pmf_cfg.required = false;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;

    svc_settings_set_str(NET_NS, "ssid", creds->ssid);
    svc_settings_set_str(NET_NS, "pass", creds->password);

    s_user_disconnect = false;
    s_connecting = true;
    s_connect_pending = true;
    s_retry_count = 0;

    svc_event_bus_publish(SVC_EVENT_WIFI_CONNECTING, NULL, 0);
    ESP_LOGI(TAG, "connecting to %s", creds->ssid);

    err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_CONN) {
        /* 还没启动完成，或驱动正在收尾：保持 pending，
         * 交给 WIFI_EVENT_STA_START / 断线回调里补连 */
        ESP_LOGI(TAG, "connect deferred (%s)", esp_err_to_name(err));
        return ESP_OK;
    }
    if (err == ESP_OK) {
        s_connect_pending = false;
    }
    return err;
}

esp_err_t svc_net_wifi_disconnect(void)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;

    s_user_disconnect = true;
    s_connecting = false;
    return esp_wifi_disconnect();
}

esp_err_t svc_net_wifi_auto_connect(void)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;

    char ssid[33] = { 0 };
    char pass[64] = { 0 };
    svc_settings_get_str(NET_NS, "ssid", ssid, sizeof(ssid), "");
    if (ssid[0] == '\0') {
        ESP_LOGI(TAG, "no saved credentials, skip auto connect");
        return ESP_ERR_NOT_FOUND;
    }
    svc_settings_get_str(NET_NS, "pass", pass, sizeof(pass), "");
    if (pass[0] == '\0') {
        ESP_LOGW(TAG, "saved SSID '%s' has no password; only open APs can match", ssid);
    }

    esp_err_t err = svc_net_wifi_start(SVC_NET_MODE_STA);
    if (err != ESP_OK) return err;

    svc_net_wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    strlcpy(creds.ssid, ssid, sizeof(creds.ssid));
    strlcpy(creds.password, pass, sizeof(creds.password));

    return svc_net_wifi_connect(&creds);
}

esp_err_t svc_net_wifi_forget(void)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;

    svc_settings_set_str(NET_NS, "ssid", "");
    svc_settings_set_str(NET_NS, "pass", "");

    s_user_disconnect = true;
    s_connecting = false;
    s_connect_pending = false;
    s_retry_count = 0;

    esp_wifi_disconnect();
    esp_wifi_stop();
    s_started = false;

    status_lock();
    memset(&s_status, 0, sizeof(s_status));
    status_unlock();

    ESP_LOGI(TAG, "saved credentials cleared");
    return ESP_OK;
}

esp_err_t svc_net_wifi_get_saved_ssid(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    svc_settings_get_str(NET_NS, "ssid", buf, len, "");
    return (buf[0] != '\0') ? ESP_OK : ESP_ERR_NOT_FOUND;
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

    status_lock();
    *status = s_status;          /* 快照拷贝，避免读到写一半的状态 */
    status_unlock();
    return ESP_OK;
}

esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms)
{
    if (url == NULL || resp_buf == NULL || buf_len == 0) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = (int)timeout_ms,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        return err;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_FAIL;
    }

    int status = esp_http_client_get_status_code(c);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d: %s", status, url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* 循环读到缓冲满：单次 read 可能只返回一部分 */
    size_t total = 0;
    err = ESP_OK;
    while (total < buf_len - 1) {
        int rd = esp_http_client_read(c, resp_buf + total, (int)(buf_len - 1 - total));
        if (rd < 0) {
            err = ESP_FAIL;
            break;
        }
        if (rd == 0) break;                      /* 数据读完 */
        total += (size_t)rd;
    }
    resp_buf[total] = '\0';
    if (total == buf_len - 1) {
        ESP_LOGW(TAG, "response truncated to %u bytes", (unsigned)total);
    }

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms)
{
    if (url == NULL || body == NULL || resp_buf == NULL || buf_len == 0) return ESP_ERR_INVALID_ARG;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = (int)timeout_ms,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) return ESP_FAIL;

    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(c);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP %d: %s", status, url);
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            size_t total = 0;
            while (total < buf_len - 1) {
                int rd = esp_http_client_read_response(c, resp_buf + total,
                                                       (int)(buf_len - 1 - total));
                if (rd < 0) {
                    err = ESP_FAIL;
                    break;
                }
                if (rd == 0) break;
                total += (size_t)rd;
            }
            resp_buf[total] = '\0';
        }
    }

    esp_http_client_cleanup(c);
    return err;
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
