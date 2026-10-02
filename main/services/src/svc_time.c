/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Time 实现（时区 + SNTP）
 *
 * 本板没有电池 RTC，掉电后系统时间从 1970 重新开始。为了不让人每次热重启都要重新
 * 校时，这里每秒把"当前可信时间"存一份进 RTC 保留内存（RTC_NOINIT_ATTR：软件复位
 * 不丢、掉电才丢），启动时用 epoch + 本次启动已过去的毫秒数接着走，误差只有两次
 * 保存之间的那不到 1 秒。
 */

#include "svc_common.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
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
#define SVC_TIME_SAVE_MS      1000                /* 时间存 RTC 内存的间隔 */
#define SVC_TIME_SET_NS       "sys"
#define SVC_TIME_KEY_TZ       "timezone"
#define SVC_TIME_KEY_24H      "clock_24h"

/* RTC 保留内存里的时间快照：magic + 反码一起校验，避免掉电后残留数据被误认 */
typedef struct {
    uint32_t magic;
    uint32_t magic_inv;
    int64_t  epoch;        /* UTC 秒 */
} svc_time_rtc_t;

#define SVC_TIME_RTC_MAGIC    0x534c544dU   /* "SLTM" */

static RTC_NOINIT_ATTR svc_time_rtc_t s_rtc;

static bool s_synced = false;
static bool s_sntp_inited = false;
static esp_timer_handle_t s_minute_timer = NULL;
static esp_timer_handle_t s_save_timer = NULL;

static void rtc_save(int64_t epoch)
{
    s_rtc.epoch = epoch;
    s_rtc.magic_inv = ~SVC_TIME_RTC_MAGIC;
    s_rtc.magic = SVC_TIME_RTC_MAGIC;       /* magic 最后写：前面两个字段一定已写好 */
}

/* 每秒把当前时间存一份到 RTC 内存：存的是"时刻"而不是"经过时间"，
 * 所以重启后直接用 epoch + 本次启动已过去的毫秒数即可，误差只有两次保存之间的那点 */
static void save_timer_cb(void *arg)
{
    (void)arg;
    if (s_synced) {
        rtc_save(svc_time_now());
    }
}

static void minute_timer_cb(void *arg)
{
    (void)arg;
    svc_event_bus_publish(SVC_EVENT_TIME_CHANGED, NULL, 0);
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    rtc_save(svc_time_now());
    ESP_LOGI(TAG, "time synced");
    svc_event_bus_publish(SVC_EVENT_TIME_SYNCED, NULL, 0);
}

/* 网络就绪后立即补一次 NTP，避免只靠 6 小时轮询 */
static void wifi_connected_cb(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    svc_time_sync_ntp();
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
        /* 并发调用时可能已被别的任务初始化，INVALID_STATE 视为成功 */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
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
    svc_settings_set_str(SVC_TIME_SET_NS, SVC_TIME_KEY_TZ, tz);
    svc_event_bus_publish(SVC_EVENT_TIMEZONE_CHANGED, NULL, 0);
    ESP_LOGI(TAG, "timezone set to %s", tz);
    return ESP_OK;
}

esp_err_t svc_time_get_timezone(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    return svc_settings_get_str(SVC_TIME_SET_NS, SVC_TIME_KEY_TZ, buf, len, SVC_TIME_DEFAULT_TZ);
}

bool svc_time_get_24h(void)
{
    uint8_t v = 1;
    svc_settings_get_u8(SVC_TIME_SET_NS, SVC_TIME_KEY_24H, &v, 1);
    return v != 0;
}

esp_err_t svc_time_set_24h(bool on)
{
    return svc_settings_set_u8(SVC_TIME_SET_NS, SVC_TIME_KEY_24H, on ? 1 : 0);
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
    rtc_save(ts);
    svc_event_bus_publish(SVC_EVENT_TIME_SYNCED, NULL, 0);
    return ESP_OK;
}

/* 联网后自动校时 + 定时同步：
 *   - 连上 Wi-Fi 时 wifi_connected_cb() 立刻触发一次（见 svc_time_init）
 *   - 这里每 6 小时兜底同步一次（SNTP 客户端自身也会按 CONFIG_LWIP_SNTP_UPDATE_DELAY 轮询）
 *   - 手动校时把时间设成"已同步"也不会挡住 NTP：有网络时 NTP 会把它校准回来
 * 无网络时 svc_time_sync_ntp() 直接返回 INVALID_STATE，不开 SNTP。 */
static void ntp_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        svc_time_sync_ntp();
        vTaskDelay(pdMS_TO_TICKS(SVC_TIME_RESYNC_MS));
    }
}

esp_err_t svc_time_init(void)
{
    char tz[32];
    svc_settings_get_str(SVC_TIME_SET_NS, SVC_TIME_KEY_TZ, tz, sizeof(tz), SVC_TIME_DEFAULT_TZ);
    setenv("TZ", tz, 1);
    tzset();

    /* 热重启（软件复位 / 看门狗 / panic）后接着上次的可信时间走。
     * 掉电后 RTC 内存失效，magic 校验会失败，系统时间仍是 1970。
     * 误差 = 最后一次保存到复位之间的那点时间（不到 1 秒）。 */
    if (s_rtc.magic == SVC_TIME_RTC_MAGIC && s_rtc.magic_inv == ~SVC_TIME_RTC_MAGIC) {
        const int64_t boot_us = esp_timer_get_time();
        const int64_t epoch = s_rtc.epoch + boot_us / 1000000;
        struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
            s_synced = true;
            ESP_LOGI(TAG, "time restored after reboot (epoch %lld)", (long long)epoch);
        }
    }

    /* 每秒把当前时间存进 RTC 内存（未同步时不存，避免存下 1970 的假时间） */
    const esp_timer_create_args_t sargs = {
        .callback = save_timer_cb,
        .arg = NULL,
        .name = "time_save",
    };
    if (esp_timer_create(&sargs, &s_save_timer) == ESP_OK) {
        esp_timer_start_periodic(s_save_timer, SVC_TIME_SAVE_MS * 1000ULL);
    }

    xTaskCreatePinnedToCore(ntp_sync_task, "ntp_sync_task", 3072, NULL, 2, NULL, 0);

    /* 每分钟发布 SVC_EVENT_TIME_CHANGED（供状态栏等处刷新） */
    const esp_timer_create_args_t targs = {
        .callback = minute_timer_cb,
        .arg = NULL,
        .name = "time_minute",
    };
    if (esp_timer_create(&targs, &s_minute_timer) == ESP_OK) {
        esp_timer_start_periodic(s_minute_timer, 60ULL * 1000 * 1000);
    }

    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, wifi_connected_cb, NULL);

    ESP_LOGI(TAG, "initialized (tz=%s)", tz);
    return ESP_OK;
}
