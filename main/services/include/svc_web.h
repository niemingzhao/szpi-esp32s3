/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 局域网 Web 管理页
 *
 * 设备连上 Wi-Fi（拿到 IP）后自动在 80 端口起 HTTP 服务、断开自动停：同一个局域网里的
 * 电脑 / 手机浏览器打开 http://<设备 IP>/ 就能用「系统状态」和「文件管理」两个页签。
 *
 * 不做认证，只在可信的局域网里用。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化（订阅 Wi-Fi 事件；调用时已经连上网就立刻起服务）
 *
 * 只在项目最开始初始化一次；重复调用是空操作。
 */
esp_err_t svc_web_init(void);

/**
 * @brief 服务是否正在监听（界面提示"浏览器访问 http://<ip>/"时用来判断）
 */
bool svc_web_is_running(void);

/**
 * @brief 自定义请求的请求信息（回调在 HTTP 任务里执行）
 */
typedef struct {
    const char *method;     /* "GET" / "POST" / "PUT" / "DELETE" 等 */
    const char *path;       /* 去掉 /s 前缀后的路径，例如 "/move"（根路径是 "/"） */
    const char *query;      /* 原始 query 串（未解码；没有则为空串） */
    const char *body;       /* 请求体（过大时截断；GET 时为空串） */
    size_t body_len;
} svc_web_req_t;

/**
 * @brief 自定义请求的响应（回调里填，回调返回后立即下发）
 *
 * body 指向调用方的缓冲，回调返回前有效即可；status 为 0 按 200 处理。
 */
typedef struct {
    int status;
    const char *content_type;
    const void *body;
    size_t body_len;
} svc_web_resp_t;

/** 自定义请求处理回调：返回非 ESP_OK 表示处理失败（按 500 下发） */
typedef esp_err_t (*svc_web_custom_cb_t)(const svc_web_req_t *req, svc_web_resp_t *resp, void *user);

/**
 * @brief 注册 /s 前缀下的请求处理回调（同一时刻只允许一个）
 *
 * 给脚本运行时用：脚本注册的网页路由都挂在 /s 底下，浏览器访问
 * http://<设备 IP>/s/...。回调跑在 esp_http_server 自己的任务里，必须尽快返回 ——
 * 它一阻塞，整个 Web 控制台（含状态轮询、文件列表）都跟着等。没有注册或返回
 * ESP_ERR_NOT_FOUND 时按 404 下发。
 */
esp_err_t svc_web_set_custom_handler(svc_web_custom_cb_t cb, void *user);

/** 取消注册（脚本停止时调） */
void svc_web_clear_custom_handler(void);

/**
 * @brief 临时挂起 / 恢复 Web 控制台
 *
 * 相机打开时调 true：先把 httpd 停掉、把它的任务栈还给内部堆，相机 DVP 才拿得到
 * 那 7.6 KB 连续内部 DMA；相机用完调 false 恢复（Wi-Fi 仍连着的话会自动重起）。
 */
void svc_web_set_hold(bool hold);

#ifdef __cplusplus
}
#endif
