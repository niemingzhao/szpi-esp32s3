/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - OTA（HTTPS 固件升级）
 *
 * 分区：factory / ota_0 / ota_1（见 partitions.csv）。升级在独立任务里跑，
 * 界面通过 svc_ota_progress() 轮询进度；成功后自动重启。
 * 只允许 HTTPS：http_config 必须挂 esp_crt_bundle_attach，且
 * CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP 保持关闭。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 开始后台 OTA（url 必须是 https://…）
 *
 * 下载完成后写 OTA 分区、校验、设置启动分区并重启；失败时状态回到 IDLE 并记日志。
 */
esp_err_t svc_ota_start(const char *url);

/**
 * @brief 请求中止当前 OTA（尽力而为，下一次循环检查到就退出）
 */
esp_err_t svc_ota_abort(void);

/**
 * @brief 是否正在升级
 */
bool svc_ota_is_running(void);

/**
 * @brief 升级进度 0-100；未在升级时返回 -1
 */
int svc_ota_progress(void);

#ifdef __cplusplus
}
#endif
