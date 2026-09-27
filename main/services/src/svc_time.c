/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 时间实现（时区 + SNTP）
 *
 * 时区（POSIX 串 + 城市显示名）一起存在 NVS 的 sys/timezone（svc_time_tz_t）。
 *
 * 本板没有电池 RTC，掉电后系统时间从 1970 重新开始。两层保时间：
 *   1) RTC 保留内存（RTC_NOINIT_ATTR，每秒写）：软件复位 / 看门狗 / panic 后接着走，
 *      误差不到 1 秒；掉电会丢。
 *   2) NVS（sys/time_epoch，每分钟 + 同步 / 手动校时后写）：掉电也不丢，代价是掉电重启
 *      后最多偏 1 分钟（两次写入之间的间隔）。
 * 开机时取两者中较晚且有效的那个（≥ 2020-09），并置为"已同步"。
 * 绝不能每秒写 NVS：那是写放大，而且每次 nvs_commit 都会阻塞调用任务。
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
#define SVC_TIME_NVS_MS       (60 * 1000)         /* 时间存 NVS 的间隔（掉电重启误差上限） */
#define SVC_TIME_SET_NS       "sys"
#define SVC_TIME_KEY_TZ       "timezone"
#define SVC_TIME_KEY_24H      "clock_24h"
#define SVC_TIME_KEY_EPOCH    "time_epoch"
#define SVC_TIME_MIN_VALID    1600000000LL        /* 2020-09-13：低于它视为没同步过的假时间 */

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

/* 时间落 NVS：掉电也不丢（RTC 内存只扛软复位）。只在有效时间上写，
 * 且按分钟级写 —— 绝不能每秒写 flash（写放大会拖慢调用任务） */
static void nvs_save(int64_t epoch)
{
    if (epoch < SVC_TIME_MIN_VALID) return;

    if (svc_settings_set_blob(SVC_TIME_SET_NS, SVC_TIME_KEY_EPOCH,
                              &epoch, sizeof(epoch)) != ESP_OK) {
        ESP_LOGW(TAG, "save time to nvs failed");
    }
}

/* 读 NVS 里的时间；没存过 / 长度不符 / 值无效时返回 false */
static bool nvs_load(int64_t *epoch)
{
    if (epoch == NULL) return false;

    int64_t v = 0;
    size_t len = sizeof(v);
    if (svc_settings_get_blob(SVC_TIME_SET_NS, SVC_TIME_KEY_EPOCH, &v, &len) != ESP_OK) {
        return false;
    }
    if (len != sizeof(v) || v < SVC_TIME_MIN_VALID) return false;

    *epoch = v;
    return true;
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
    nvs_save(svc_time_now());       /* 校时后立刻落一次 NVS，防刚同步就掉电 */
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

esp_err_t svc_time_set_timezone(const svc_time_tz_t *tz)
{
    if (tz == NULL || tz->tz[0] == '\0') return ESP_ERR_INVALID_ARG;

    setenv("TZ", tz->tz, 1);
    tzset();

    esp_err_t err = svc_settings_set_blob(SVC_TIME_SET_NS, SVC_TIME_KEY_TZ, tz, sizeof(*tz));
    if (err != ESP_OK) ESP_LOGW(TAG, "save timezone failed: %s", esp_err_to_name(err));

    svc_event_bus_publish(SVC_EVENT_TIMEZONE_CHANGED, NULL, 0);
    ESP_LOGI(TAG, "timezone set to %s (%s)", tz->tz, tz->name);
    return err;
}

esp_err_t svc_time_get_timezone(svc_time_tz_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    /* 键不存在、类型不符（旧固件存的是字符串）或内容为空都回落默认时区 */
    size_t len = sizeof(*out);
    if (svc_settings_get_blob(SVC_TIME_SET_NS, SVC_TIME_KEY_TZ, out, &len) != ESP_OK ||
        len != sizeof(*out) || out->tz[0] == '\0') {
        memset(out, 0, sizeof(*out));
        strlcpy(out->tz, SVC_TIME_DEFAULT_TZ, sizeof(out->tz));
    }
    return ESP_OK;
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
    nvs_save(ts);
    svc_event_bus_publish(SVC_EVENT_TIME_SYNCED, NULL, 0);
    return ESP_OK;
}

/* 联网后自动校时 + 定时同步 + 定时落 NVS：
 *   - 连上 Wi-Fi 时 wifi_connected_cb() 立刻触发一次（见 svc_time_init）
 *   - 每 6 小时兜底同步一次（SNTP 客户端自身也会按 CONFIG_LWIP_SNTP_UPDATE_DELAY 轮询）
 *   - 每分钟把当前可信时间写一份进 NVS（掉电重启后最多偏 SVC_TIME_NVS_MS）
 *   - 手动校时把时间设成"已同步"也不会挡住 NTP：有网络时 NTP 会把它校准回来
 * 无网络时 svc_time_sync_ntp() 直接返回 INVALID_STATE，不开 SNTP。 */
static void ntp_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));

    uint32_t since_sync = 0;
    while (true) {
        if (s_synced) nvs_save(svc_time_now());

        since_sync += SVC_TIME_NVS_MS;
        if (since_sync >= SVC_TIME_RESYNC_MS) {
            since_sync = 0;
            svc_time_sync_ntp();
        }
        vTaskDelay(pdMS_TO_TICKS(SVC_TIME_NVS_MS));
    }
}

esp_err_t svc_time_init(void)
{
    svc_time_tz_t tz;
    svc_time_get_timezone(&tz);
    setenv("TZ", tz.tz, 1);
    tzset();

    /* 恢复时间：RTC 内存（软复位，最准）与 NVS（掉电也不丢）取较晚且有效的那个。
     * 未同步过 / 掉电后残留的无效值会被 SVC_TIME_MIN_VALID 挡掉，系统时间保持 1970。 */
    int64_t restored = 0;
    bool have_time = false;

    if (s_rtc.magic == SVC_TIME_RTC_MAGIC && s_rtc.magic_inv == ~SVC_TIME_RTC_MAGIC &&
        s_rtc.epoch >= SVC_TIME_MIN_VALID) {
        const int64_t boot_us = esp_timer_get_time();
        restored = s_rtc.epoch + boot_us / 1000000;
        have_time = true;
    }

    int64_t nvs_epoch = 0;
    if (nvs_load(&nvs_epoch)) {
        ESP_LOGI(TAG, "time from nvs: epoch %lld", (long long)nvs_epoch);
        if (!have_time || nvs_epoch > restored) {
            restored = nvs_epoch;
            have_time = true;
        }
    }

    if (have_time) {
        struct timeval tv = { .tv_sec = (time_t)restored, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
            s_synced = true;
            ESP_LOGI(TAG, "time restored (epoch %lld)", (long long)restored);
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

    if (xTaskCreatePinnedToCore(ntp_sync_task, "ntp_sync_task", 4096, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "ntp sync task create failed");
    }

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

    ESP_LOGI(TAG, "initialized (tz=%s)", tz.tz);
    return ESP_OK;
}
