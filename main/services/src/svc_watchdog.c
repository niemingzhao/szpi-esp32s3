/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 看门狗实现（Task WDT）
 *
 * 监控关键任务，喂狗超时（CONFIG_ESP_TASK_WDT_TIMEOUT_S，默认 5 s）自动重启。
 *
 * 关键任务在自己的循环里 svc_watchdog_subscribe() + svc_watchdog_feed()；
 * 启动阶段先"只告警"运行，main 在启动末尾调 svc_watchdog_arm() 后才打开自动重启
 * （首次 SPIFFS 格式化会长时间不喂狗）。
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "sdkconfig.h"

static const char *TAG = "svc.watchdog";

#ifndef CONFIG_ESP_TASK_WDT_TIMEOUT_S
#define CONFIG_ESP_TASK_WDT_TIMEOUT_S 5
#endif

#define WDT_TIMEOUT_MS   ((uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U)

static bool s_armed = false;

static esp_err_t wdt_apply(uint32_t timeout_ms, bool panic)
{
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1U,
        .trigger_panic = panic,
    };

    /* 默认配置下 TWDT 已由 IDF 在启动时初始化，reconfigure 优先；
     * 只有 CONFIG_ESP_TASK_WDT_INIT 关闭时才需要自己拉起 */
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&cfg);
    }
    return err;
}

esp_err_t svc_watchdog_init(void)
{
    esp_err_t err = wdt_apply(WDT_TIMEOUT_MS, s_armed);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "configure failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "initialized (timeout=%u ms, auto-reboot=%s)",
             (unsigned)WDT_TIMEOUT_MS, s_armed ? "on" : "off");
    return ESP_OK;
}

esp_err_t svc_watchdog_arm(void)
{
    esp_err_t err = wdt_apply(WDT_TIMEOUT_MS, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "arm failed: %s", esp_err_to_name(err));
        return err;
    }

    s_armed = true;
    ESP_LOGI(TAG, "armed: timeout %u ms -> auto reboot", (unsigned)WDT_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t svc_watchdog_disarm(void)
{
    esp_err_t err = wdt_apply(WDT_TIMEOUT_MS, false);
    if (err != ESP_OK) return err;

    s_armed = false;
    ESP_LOGW(TAG, "disarmed: timeout only logs");
    return ESP_OK;
}

esp_err_t svc_watchdog_subscribe(void)
{
    if (esp_task_wdt_status(NULL) == ESP_OK) return ESP_OK;   /* 已纳入 */
    return esp_task_wdt_add(NULL);
}

esp_err_t svc_watchdog_feed(void)
{
    /* 未纳入监控的任务（如 script_task）调用时直接返回：esp_task_wdt_reset()
     * 会打一条 "task not found" 错误日志，20 ms 一圈的任务会把它刷成串口洪水。
     * 有了这道判断，feed() 对任何任务都是安全的（未纳入 = 无需喂狗）。 */
    if (esp_task_wdt_status(NULL) != ESP_OK) return ESP_ERR_INVALID_STATE;
    return esp_task_wdt_reset();
}
