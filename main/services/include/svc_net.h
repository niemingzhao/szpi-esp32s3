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

/** AP/Web 配网：开热点 + HTTP 配置页（浏览器访问 http://192.168.4.1/），连上目标 AP 后自动关闭 */
esp_err_t svc_net_prov_start(const char *ap_ssid, const char *ap_password);
esp_err_t svc_net_prov_stop(void);
bool svc_net_prov_is_active(void);

/** 状态查询 */
esp_err_t svc_net_get_status(svc_net_status_t *status);

/** HTTP 客户端 */
esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms);

/** 异步 GET 的回调（在 svc.http 任务里执行，不是 LVGL 任务，界面必须自己加 LVGL 锁） */
typedef void (*svc_http_cb_t)(const char *body, esp_err_t err, void *user);

/**
 * @brief 异步 HTTP GET（自己起一个临时任务，避免阻塞调用者）
 *
 * 请求完成（或失败）后调用 cb(body, err, user)；body 就是 resp_buf，失败时为空串。
 * http:// 与 https:// 都支持（https 用内置证书 bundle 校验，见 sdkconfig.defaults）。
 */
esp_err_t svc_http_get_async(const char *url, char *resp_buf, size_t buf_len,
                             svc_http_cb_t cb, void *user);

/**
 * @brief 下载进度 / 结束回调（在 svc.http 任务里执行，不是 LVGL 任务，界面必须自己加 LVGL 锁）
 *
 * 传输过程中会多次回调（err = ESP_OK、done = false）；下载结束时一定额外回调一次，
 * 此时 done = true、err 为最终结果（成功 ESP_OK，失败为具体错误码），此后不再回调。
 * received 为已写入文件的字节数；total 为 Content-Length，未知（如分块传输）时传 0。
 */
typedef void (*svc_http_progress_cb_t)(size_t received, size_t total, esp_err_t err,
                                       bool done, void *user);

/**
 * @brief 异步下载到文件（自己起一个临时任务，边收边写，支持二进制）
 *
 * 按实际收到的字节数写文件（不用 strlen，含 NUL 的二进制不会被截断），失败时删除半截文件。
 * 结束时回调 cb(..., done = true, ...)；调用方无需提供内存缓冲。
 */
esp_err_t svc_http_download(const char *url, const char *path,
                            svc_http_progress_cb_t cb, void *user);

/** MQTT / WebSocket 已拆成独立服务：见 svc_mqtt.h、svc_ws.h */

#ifdef __cplusplus
}
#endif
