/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Time 实现（时区 + SNTP）
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "svc.time";

#define SVC_TIME_DEFAULT_TZ   "CST-8"
#define SVC_TIME_NTP_SERVER   "pool.ntp.org"
#define SVC_TIME_RESYNC_MS    (6 * 3600 * 1000)   /* 每 6 小时 */

static bool s_synced = false;
static bool s_sntp_inited = false;

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "time synced");
    svc_event_bus_publish(SVC_EVENT_TIME_SYNCED, NULL, 0);
}

esp_err_t svc_time_sync_ntp(void)
{
    /* 无网络接口时直接失败，避免在无 netif 的情况下初始化 SNTP */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_sntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SVC_TIME_NTP_SERVER);
        cfg.sync_cb = time_sync_cb;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_sntp_inited = true;
    } else {
        esp_netif_sntp_start();
    }
    return ESP_OK;
}

esp_err_t svc_time_set_timezone(const char *tz)
{
    if (tz == NULL) return ESP_ERR_INVALID_ARG;

    setenv("TZ", tz, 1);
    tzset();
    svc_settings_set_str("sys", "timezone", tz);
    svc_event_bus_publish(SVC_EVENT_TIMEZONE_CHANGED, NULL, 0);
    ESP_LOGI(TAG, "timezone set to %s", tz);
    return ESP_OK;
}

bool svc_time_is_synced(void)
{
    return s_synced;
}

int64_t svc_time_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

esp_err_t svc_time_format(int64_t ts, const char *fmt, char *buf, size_t len)
{
    if (fmt == NULL || buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    time_t t = (time_t)ts;
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (strftime(buf, len, fmt, &tmv) == 0) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t svc_time_set_manual(int64_t ts)
{
    struct timeval tv = { .tv_sec = (time_t)ts, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;

    s_synced = true;
    svc_event_bus_publish(SVC_EVENT_TIME_SYNCED, NULL, 0);
    return ESP_OK;
}

static void ntp_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        if (!s_synced) {
            svc_time_sync_ntp();
        }
        vTaskDelay(pdMS_TO_TICKS(SVC_TIME_RESYNC_MS));
    }
}

esp_err_t svc_time_init(void)
{
    char tz[32];
    svc_settings_get_str("sys", "timezone", tz, sizeof(tz), SVC_TIME_DEFAULT_TZ);
    setenv("TZ", tz, 1);
    tzset();

    xTaskCreate(ntp_sync_task, "ntp_sync_task", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "initialized (tz=%s)", tz);
    return ESP_OK;
}
