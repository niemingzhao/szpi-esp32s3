/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 网络（Wi-Fi STA / 配网 / HTTP 客户端）
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
 * @brief 网络模式（公开接口只切换 OFF / STA；AP / STA_AP 由配网流程内部使用）
 */
typedef enum {
    SVC_NET_MODE_OFF,
    SVC_NET_MODE_STA,
    SVC_NET_MODE_AP,
    SVC_NET_MODE_STA_AP,
} svc_net_mode_t;

/* AP / Web 配网热点名由 svc_identity_ap_ssid() 生成（系统名-地址后 4 位），
 * 界面提示里显示，连上它再访问 http://192.168.4.1/ */

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

/** 初始化（建 STA 网卡与事件处理；有保存的凭据时自动重连） */
esp_err_t svc_net_init(void);

/** 启动 Wi-Fi（只支持 OFF / STA；已在该模式时为空操作） */
esp_err_t svc_net_wifi_start(svc_net_mode_t mode);
esp_err_t svc_net_wifi_stop(void);

/**
 * @brief 扫描 AP（阻塞到扫描结束或超时；必要时会先启动 STA）
 *
 * 结果按 max_aps 截断，found 返回实际数量；不要在 LVGL 任务里直接调用。
 */
esp_err_t svc_net_wifi_scan(svc_net_wifi_ap_t *aps, size_t max_aps, size_t *found, uint32_t timeout_ms);

/** 连接（已知 SSID；凭据会持久化，供下次开机自动重连） */
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

/** 状态查询（内部加锁取快照，可跨任务调用） */
esp_err_t svc_net_get_status(svc_net_status_t *status);

/** HTTP 客户端（阻塞；https 走内置证书 bundle 校验） */
esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms);

/** 额外请求头 / 请求体的长度上限 */
#define SVC_HTTP_HEADERS_MAX   512
#define SVC_HTTP_BODY_MAX      2048

/**
 * @brief 一次 HTTP(S) 请求的描述（method 为 NULL 时按 GET）
 *
 * headers 是 "K: V\r\nK: V" 形式的额外请求头（可 NULL），用来带 Authorization 等；
 * 有 body 时必须给 body_len，content_type 可 NULL（默认 text/plain）。
 */
typedef struct {
    const char *method;         /* "GET" / "POST" / "PUT" / "DELETE" / "PATCH" / "HEAD" */
    const char *url;
    const char *content_type;
    const char *body;
    size_t body_len;
    const char *headers;
} svc_http_request_t;

/**
 * @brief 同步 HTTP(S) 请求（阻塞；供脚本绑定这类能等的调用者用）
 *
 * 响应体写进 resp_buf（截断并告警），返回 ESP_OK 表示 2xx 且已读完。
 */
esp_err_t svc_http_request(const svc_http_request_t *req, char *resp_buf, size_t buf_len,
                           uint32_t timeout_ms);

/**
 * @brief 最后一次请求的 HTTP 状态码（0 = 还没拿到响应）
 *
 * 失败时用它区分"服务器拒绝"（4xx / 5xx）和"设备侧错误"（0）。
 */
int svc_http_last_status(void);

/** 异步 GET 的回调（在 svc.http 任务里执行，不是 LVGL 任务，界面必须自己加 LVGL 锁） */
typedef void (*svc_http_cb_t)(const char *body, esp_err_t err, void *user);

/**
 * @brief 异步 HTTP GET（自己起一个临时任务，避免阻塞调用者）
 *
 * 请求完成（或失败）后调用 cb(body, err, user)；body 就是 resp_buf，失败时为空串。
 * http:// 与 https:// 都支持（https 用内置证书 bundle 校验）。
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
 * @brief 落盘前的"定名字"回调（可选，可为 NULL）
 *
 * 服务把响应里能拿到的文件名交给它：优先 Content-Disposition 的 filename=
 * （含 filename*= 形式），其次是重定向之后最终 URL 的最后一段路径；两者都没有时
 * 给空串。调用方据此决定目录 / 扩展名（例如 .lua 落到脚本目录、没有名字时用默认名），
 * 把最终路径写进 out_path。返回非 ESP_OK 表示这次下载失败。
 *
 * 在 svc.http 任务里执行（拿到响应头之后、创建文件之前），不要在这里碰界面。
 */
typedef esp_err_t (*svc_http_name_cb_t)(const char *name, char *out_path, size_t path_len,
                                        void *user);

/**
 * @brief 异步下载到文件（自己起一个临时任务，边收边写，支持二进制）
 *
 * 按实际收到的字节数写文件（不用 strlen，含 NUL 的二进制不会被截断），失败时删除半截文件。
 * path 是兜底的落盘路径；给了 name_cb 时由它按响应里的文件名决定最终路径。
 * 结束时回调 cb(..., done = true, ...)；调用方无需提供内存缓冲。
 */
esp_err_t svc_http_download(const char *url, const char *path, svc_http_progress_cb_t cb,
                            svc_http_name_cb_t name_cb, void *user);

/**
 * @brief 下载状态
 */
typedef enum {
    SVC_HTTP_DL_IDLE,       /* 没有下载 */
    SVC_HTTP_DL_RUNNING,    /* 传输中 */
    SVC_HTTP_DL_PAUSED,     /* 停在半截上，等 svc_http_download_resume() */
} svc_http_dl_state_t;

svc_http_dl_state_t svc_http_download_state(void);

/** 已下载（已写入文件）的字节数：显示进度、也是暂停后续传的起点 */
size_t svc_http_download_received(void);

/** 最后一次响应的 HTTP 状态码（0 = 还没拿到响应）：失败时用来区分"服务器拒绝"和"设备错误" */
int svc_http_download_status(void);

/**
 * @brief 暂停当前下载
 *
 * 任务在下一次进度检查时收尾，随后回调 done = true（err = ESP_OK），状态变成
 * SVC_HTTP_DL_PAUSED，半截文件与已收字节数都留着，等 svc_http_download_resume() 接着下。
 */
esp_err_t svc_http_download_pause(void);

/**
 * @brief 继续被暂停的下载
 *
 * 带 Range: bytes=N- 续传；服务端返回 200（不认 Range）时自动从头重下。
 */
esp_err_t svc_http_download_resume(void);

/**
 * @brief 停止（取消）当前下载：停下并删掉半截文件
 */
esp_err_t svc_http_download_cancel(void);

/** MQTT / WebSocket 已拆成独立服务：见 svc_mqtt.h、svc_ws.h */

#ifdef __cplusplus
}
#endif
