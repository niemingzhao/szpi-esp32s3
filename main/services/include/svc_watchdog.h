/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 看门狗（Task WDT）
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动看门狗服务（重复调用安全）
 *
 * 喂狗超时取 CONFIG_ESP_TASK_WDT_TIMEOUT_S（默认 5 s），监控两个核的 idle 任务。
 * 此时仍是"只告警"模式：启动阶段有首次 SPIFFS 格式化这类长时间不喂狗的操作，
 * 直接打开自动重启会误触发。
 */
esp_err_t svc_watchdog_init(void);

/**
 * @brief 打开"喂狗超时自动重启"，应在启动流程末尾调用
 */
esp_err_t svc_watchdog_arm(void);

/**
 * @brief 退回"只告警"模式（长时间不让出 CPU 的操作前临时用，如格式化）
 */
esp_err_t svc_watchdog_disarm(void);

/**
 * @brief 把当前任务纳入 Task WDT（幂等，已纳入时返回 ESP_OK）
 *
 * 纳入后任务必须在超时时间内调用 svc_watchdog_feed()，否则看门狗会告警 /
 * 重启。只应纳入循环周期远小于超时的任务。
 */
esp_err_t svc_watchdog_subscribe(void);

/**
 * @brief 当前任务喂狗
 *
 * 只在当前任务已纳入 Task WDT 时才真正喂狗；未纳入时返回 ESP_ERR_INVALID_STATE
 * 且不打日志，因此任何任务都可以安全调用。
 */
esp_err_t svc_watchdog_feed(void);

#ifdef __cplusplus
}
#endif
