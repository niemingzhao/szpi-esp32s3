/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Network 实现
 *
 * 已支持：Wi-Fi STA（启动 / 停止 / 扫描 / 连接 / 断开 / 自动重连 / 状态）、
 *         凭据持久化（NVS 命名空间 wifi）、事件发布、连接成功后触发 SNTP、
 *         HTTP GET / POST、SmartConfig 配网、AP + HTTP 配网页。
 * MQTT / WebSocket 在各自的服务里（svc_mqtt / svc_ws）。
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_smartconfig.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
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
static volatile bool s_smartconfig = false;      /* SmartConfig 配网中 */
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

/* ------------------------------- SmartConfig ------------------------------- */

/* 手机 App（ESPTouch）把 SSID / 密码广播出来，这里收到后直接连接并持久化 */
static void handle_sc_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id == SC_EVENT_GOT_SSID_PSWD) {
        const smartconfig_event_got_ssid_pswd_t *e =
            (const smartconfig_event_got_ssid_pswd_t *)data;

        svc_net_wifi_creds_t creds;
        memset(&creds, 0, sizeof(creds));
        strlcpy(creds.ssid, (const char *)e->ssid, sizeof(creds.ssid));
        strlcpy(creds.password, (const char *)e->password, sizeof(creds.password));
        ESP_LOGI(TAG, "smartconfig got ssid: %s", creds.ssid);

        esp_smartconfig_stop();
        s_smartconfig = false;
        svc_net_wifi_connect(&creds);       /* 内部会持久化凭据 */
    } else if (id == SC_EVENT_SEND_ACK_DONE) {
        ESP_LOGI(TAG, "smartconfig finished");
        esp_smartconfig_stop();
        s_smartconfig = false;
    }
}

esp_err_t svc_net_smartconfig_start(void)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;
    if (s_smartconfig) return ESP_OK;

    /* 配网期间 STA 必须在跑（监听手机广播） */
    esp_err_t err = svc_net_wifi_start(SVC_NET_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID, handle_sc_event, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "smartconfig handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    if (err != ESP_OK) return err;

    const smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    err = esp_smartconfig_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "smartconfig start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_smartconfig = true;
    ESP_LOGI(TAG, "smartconfig started (waiting for ESPTouch)");
    return ESP_OK;
}

esp_err_t svc_net_smartconfig_stop(void)
{
    if (!s_smartconfig) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_smartconfig_stop();
    s_smartconfig = false;
    ESP_LOGI(TAG, "smartconfig stopped");
    return err;
}

/* ------------------------------ AP / Web 配网 ------------------------------ */

static httpd_handle_t s_prov_httpd = NULL;
static esp_netif_t *s_prov_netif = NULL;
static volatile bool s_prov_active = false;

/* 极简 URL 解码：%XX → 字符，'+' → 空格 */
static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;

    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && src[si + 1] != '\0' && src[si + 2] != '\0') {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
}

/* 从 x-www-form-urlencoded 里取字段（值自动解码）；没找到返回 false */
static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;

    while ((p = strstr(p, key)) != NULL) {
        if ((p == body || p[-1] == '&') && p[klen] == '=') {
            const char *val = p + klen + 1;
            const char *end = strchr(val, '&');
            size_t vlen = (end != NULL) ? (size_t)(end - val) : strlen(val);

            char raw[128];
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, val, vlen);
            raw[vlen] = '\0';
            url_decode(out, out_len, raw);
            return true;
        }
        p += klen;
    }
    return false;
}

static esp_err_t prov_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>SZPI-OS 配网</title></head><body>"
        "<h3>SZPI-OS Wi-Fi 配网</h3>"
        "<form method=\"post\" action=\"/save\">"
        "SSID<br><input name=\"ssid\" maxlength=\"32\" size=\"24\"><br><br>"
        "密码<br><input name=\"password\" type=\"password\" maxlength=\"64\" size=\"24\"><br><br>"
        "<button type=\"submit\">保存并连接</button></form>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t prov_save_handler(httpd_req_t *req)
{
    char body[256];
    int limit = (req->content_len < (int)sizeof(body) - 1) ? (int)req->content_len : (int)sizeof(body) - 1;
    if (limit <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    int rd = httpd_req_recv(req, body, limit);
    if (rd <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    body[rd] = '\0';

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    form_get(body, "password", pass, sizeof(pass));

    static const char ok_page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>"
        "<h3>已保存</h3><p>SZPI-OS 正在连接该网络，可以关闭此页面。</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ok_page, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "provisioning: got ssid '%s'", ssid);

    svc_net_wifi_creds_t creds;
    memset(&creds, 0, sizeof(creds));
    strlcpy(creds.ssid, ssid, sizeof(creds.ssid));
    strlcpy(creds.password, pass, sizeof(creds.password));
    svc_net_wifi_connect(&creds);       /* 内部会持久化凭据 */

    /* 热点与 HTTP 服务在连上目标 AP（拿到 IP）后由事件回调关闭 */
    return ESP_OK;
}

/* 配网成功（收到 IP）后自动收尾：不在 Wi-Fi 事件任务里直接停 Wi-Fi */
static void prov_connected_cb(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    if (s_prov_active) {
        svc_net_prov_stop();
    }
}

esp_err_t svc_net_prov_start(const char *ap_ssid, const char *ap_password)
{
    if (!s_wifi_inited) return ESP_ERR_INVALID_STATE;
    if (s_prov_active) return ESP_OK;

    const char *ssid = (ap_ssid != NULL && ap_ssid[0] != '\0') ? ap_ssid : "SZPI-OS-Setup";

    if (s_prov_netif == NULL) {
        s_prov_netif = esp_netif_create_default_wifi_ap();
        if (s_prov_netif == NULL) return ESP_FAIL;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = (uint8_t)strlen(ssid);
    wc.ap.channel = 1;
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    if (ap_password != NULL && strlen(ap_password) >= 8) {
        strlcpy((char *)wc.ap.password, ap_password, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    /* 切到 APSTA 需要停一次再起（STA 的配置与凭据会保留） */
    esp_wifi_stop();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning AP start failed: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        s_mode = WIFI_MODE_STA;
        s_started = true;
        return err;
    }

    s_mode = WIFI_MODE_APSTA;
    s_started = true;
    s_user_disconnect = false;
    s_retry_count = 0;
    s_connect_pending = true;      /* 已有凭据时 STA 侧照常自动连接 */

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 4;
    hc.lru_purge_enable = true;
    err = httpd_start(&s_prov_httpd, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "provisioning httpd start failed: %s", esp_err_to_name(err));
        s_prov_httpd = NULL;
        return err;
    }

    const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = prov_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = prov_save_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_prov_httpd, &uri_root);
    httpd_register_uri_handler(s_prov_httpd, &uri_save);

    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, prov_connected_cb, NULL);
    s_prov_active = true;
    ESP_LOGI(TAG, "provisioning started: AP '%s' (%s), browse http://192.168.4.1/",
             ssid, (wc.ap.authmode == WIFI_AUTH_OPEN) ? "open" : "wpa2");
    return ESP_OK;
}

esp_err_t svc_net_prov_stop(void)
{
    if (!s_prov_active) return ESP_OK;

    s_prov_active = false;
    svc_event_bus_unsubscribe(SVC_EVENT_WIFI_CONNECTED, prov_connected_cb);

    if (s_prov_httpd != NULL) {
        httpd_stop(s_prov_httpd);
        s_prov_httpd = NULL;
    }

    /* 回到纯 STA 模式，继续连接目标 AP */
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_mode = WIFI_MODE_STA;
    s_started = true;

    ESP_LOGI(TAG, "provisioning stopped");
    return ESP_OK;
}

bool svc_net_prov_is_active(void)
{
    return s_prov_active;
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
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https 走内置证书 bundle */
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
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https 走内置证书 bundle */
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

/* ------------------------------ 异步 GET ------------------------------ */

#define SVC_HTTP_URL_MAX      512    /* 天气请求实测 367 字节，256 会直接 INVALID_SIZE */
#define SVC_HTTP_TASK_STACK   6144
#define SVC_HTTP_ASYNC_TIMEOUT_MS  15000

typedef struct {
    char url[SVC_HTTP_URL_MAX];
    char *buf;
    size_t buf_len;
    svc_http_cb_t cb;
    void *user;
} svc_http_req_t;

static void svc_http_task(void *arg)
{
    svc_http_req_t *req = (svc_http_req_t *)arg;

    esp_err_t err = svc_http_get(req->url, req->buf, req->buf_len, SVC_HTTP_ASYNC_TIMEOUT_MS);
    if (err != ESP_OK) req->buf[0] = '\0';

    if (req->cb != NULL) {
        req->cb(req->buf, err, req->user);
    }

    free(req);
    vTaskDelete(NULL);
}

esp_err_t svc_http_get_async(const char *url, char *resp_buf, size_t buf_len,
                             svc_http_cb_t cb, void *user)
{
    if (url == NULL || resp_buf == NULL || buf_len == 0) return ESP_ERR_INVALID_ARG;
    if (strlen(url) >= SVC_HTTP_URL_MAX) return ESP_ERR_INVALID_SIZE;

    svc_http_req_t *req = calloc(1, sizeof(*req));
    if (req == NULL) return ESP_ERR_NO_MEM;

    strlcpy(req->url, url, sizeof(req->url));
    req->buf = resp_buf;
    req->buf_len = buf_len;
    req->cb = cb;
    req->user = user;

    if (xTaskCreate(svc_http_task, "svc.http", SVC_HTTP_TASK_STACK, req, 5, NULL) != pdPASS) {
        free(req);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "http get (async): %s", url);
    return ESP_OK;
}

/* ------------------------------ 异步下载 ------------------------------ */

#define SVC_HTTP_PATH_MAX      256
#define SVC_HTTP_DL_CHUNK      2048
#define SVC_HTTP_DL_STACK      6144

typedef struct {
    char url[SVC_HTTP_URL_MAX];
    char path[SVC_HTTP_PATH_MAX];
    svc_http_progress_cb_t cb;
    void *user;
} svc_http_dl_t;

/*
 * 流式下载：按实际收到的字节写文件（不用 strlen），二进制安全。
 * total 未知（esp_http_client_fetch_headers() 返回负值，常见于分块传输）时传 0。
 */
static void svc_http_dl_task(void *arg)
{
    svc_http_dl_t *dl = (svc_http_dl_t *)arg;

    esp_err_t err = ESP_OK;
    size_t received = 0;
    size_t total = 0;
    FILE *fp = NULL;
    char *chunk = NULL;
    esp_http_client_handle_t c = NULL;

    esp_http_client_config_t cfg = {
        .url = dl->url,
        .timeout_ms = SVC_HTTP_ASYNC_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https 走内置证书 bundle */
    };

    c = esp_http_client_init(&cfg);
    if (c == NULL) {
        err = ESP_FAIL;
    } else if (esp_http_client_open(c, 0) != ESP_OK) {
        err = ESP_FAIL;
    } else {
        int64_t len = esp_http_client_fetch_headers(c);
        total = (len > 0) ? (size_t)len : 0;      /* 负值 = 长度未知，交给状态码与读取判断 */

        const int status = esp_http_client_get_status_code(c);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP %d: %s", status, dl->url);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (err == ESP_OK) {
        fp = fopen(dl->path, "wb");
        if (fp == NULL) {
            ESP_LOGE(TAG, "open %s failed", dl->path);
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        chunk = malloc(SVC_HTTP_DL_CHUNK);
        if (chunk == NULL) err = ESP_ERR_NO_MEM;
    }

    while (err == ESP_OK) {
        int rd = esp_http_client_read(c, chunk, SVC_HTTP_DL_CHUNK);
        if (rd < 0) {
            err = ESP_FAIL;
            break;
        }
        if (rd == 0) break;                       /* 数据读完 */

        if (fwrite(chunk, 1, (size_t)rd, fp) != (size_t)rd) {
            err = ESP_FAIL;
            break;
        }
        received += (size_t)rd;
        if (dl->cb != NULL) dl->cb(received, total, ESP_OK, false, dl->user);
    }

    free(chunk);
    if (fp != NULL) fclose(fp);
    if (c != NULL) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "download failed (%s): %s", esp_err_to_name(err), dl->url);
        remove(dl->path);                         /* 失败不留下半截文件 */
    } else {
        ESP_LOGI(TAG, "downloaded %u bytes -> %s", (unsigned)received, dl->path);
    }

    if (dl->cb != NULL) dl->cb(received, total, err, true, dl->user);

    free(dl);
    vTaskDelete(NULL);
}

esp_err_t svc_http_download(const char *url, const char *path,
                            svc_http_progress_cb_t cb, void *user)
{
    if (url == NULL || path == NULL) return ESP_ERR_INVALID_ARG;
    if (strlen(url) >= SVC_HTTP_URL_MAX) return ESP_ERR_INVALID_SIZE;
    if (strlen(path) >= SVC_HTTP_PATH_MAX) return ESP_ERR_INVALID_SIZE;

    svc_http_dl_t *dl = calloc(1, sizeof(*dl));
    if (dl == NULL) return ESP_ERR_NO_MEM;

    strlcpy(dl->url, url, sizeof(dl->url));
    strlcpy(dl->path, path, sizeof(dl->path));
    dl->cb = cb;
    dl->user = user;

    if (xTaskCreate(svc_http_dl_task, "svc.http", SVC_HTTP_DL_STACK, dl, 5, NULL) != pdPASS) {
        free(dl);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "http download (async): %s -> %s", url, path);
    return ESP_OK;
}
