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
 * @brief 读取当前时区（POSIX TZ 字符串；未设置过时返回 "CST-8"）
 */
esp_err_t svc_time_get_timezone(char *buf, size_t len);

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

/**
 * @brief 时间格式（12 / 24 小时制）设置
 *
 * 存在 NVS 的 sys/clock_24h；状态栏与时钟 App 共用同一个设置，所以在时钟 App 里
 * 切到 12 小时制后，状态栏时间也会变成 12 小时制。
 */
bool svc_time_get_24h(void);
esp_err_t svc_time_set_24h(bool on);

#ifdef __cplusplus
}
#endif
