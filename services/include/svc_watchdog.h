/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 看门狗（Task Watchdog）
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动看门狗服务（重复调用安全）
 *
 * 设置喂狗超时（默认取 CONFIG_ESP_TASK_WDT_TIMEOUT_S，即 5 s）与监控核。
 * 此时仍是"只告警"模式：启动阶段有首次 SPIFFS 格式化这类长时间不喂狗的
 * 操作，直接打开自动重启会误触发。
 */
esp_err_t svc_watchdog_init(void);

/**
 * @brief 打开"喂狗超时自动重启"（PRD SYS-004），应在启动流程末尾调用
 */
esp_err_t svc_watchdog_arm(void);

/**
 * @brief 退回"只告警"模式（调试用）
 */
esp_err_t svc_watchdog_disarm(void);

/**
 * @brief 是否已打开自动重启
 */
bool svc_watchdog_is_armed(void);

/**
 * @brief 把当前任务纳入 Task WDT（幂等，已纳入时返回 ESP_OK）
 *
 * 纳入后任务必须在超时时间内调用 svc_watchdog_feed()，否则看门狗会告警 /
 * 重启。只应纳入循环周期远小于超时的任务。
 */
esp_err_t svc_watchdog_subscribe(void);

/**
 * @brief 当前任务喂狗
 */
esp_err_t svc_watchdog_feed(void);

/**
 * @brief 当前任务是否已纳入 Task WDT
 */
bool svc_watchdog_is_subscribed(void);

/**
 * @brief 设置喂狗超时（毫秒，下限 1000 ms）
 */
esp_err_t svc_watchdog_set_timeout(uint32_t timeout_ms);

/**
 * @brief 读取当前喂狗超时（毫秒）
 */
uint32_t svc_watchdog_get_timeout(void);

#ifdef __cplusplus
}
#endif
