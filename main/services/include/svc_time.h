/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Time
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
 * @brief 初始化时间服务（默认时区 CST-8）
 */
esp_err_t svc_time_init(void);

/**
 * @brief 触发一次 SNTP 同步（非阻塞；需已有网络接口）
 */
esp_err_t svc_time_sync_ntp(void);

/**
 * @brief 设置时区（POSIX TZ 字符串，如 "CST-8"）
 */
esp_err_t svc_time_set_timezone(const char *tz);

/**
 * @brief 是否已完成过 NTP 同步
 */
bool svc_time_is_synced(void);

/**
 * @brief 获取 epoch 秒
 */
int64_t svc_time_now(void);

/**
 * @brief 格式化时间（fmt 同 strftime）
 */
esp_err_t svc_time_format(int64_t ts, const char *fmt, char *buf, size_t len);

/**
 * @brief 手动设置时间
 */
esp_err_t svc_time_set_manual(int64_t ts);

#ifdef __cplusplus
}
#endif
