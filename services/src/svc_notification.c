/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Notification 实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "svc.notification";

#define SVC_NOTI_MAX        16
#define SVC_NOTI_TITLE_MAX  64
#define SVC_NOTI_MSG_MAX    128

typedef struct {
    bool used;
    svc_notification_t noti;
    char title[SVC_NOTI_TITLE_MAX];
    char message[SVC_NOTI_MSG_MAX];
} svc_noti_slot_t;

static svc_noti_slot_t s_slots[SVC_NOTI_MAX];
static uint32_t s_next_id = 1;
static SemaphoreHandle_t s_mux = NULL;      /* 保护 s_slots（LVGL 任务与其他任务都会访问） */

/* 最近一次 POST 的事件负载副本：payload 指向这里，而不是随时可能被 dismiss 的槽位 */
static svc_notification_t s_last_posted;
static char s_last_title[SVC_NOTI_TITLE_MAX];
static char s_last_msg[SVC_NOTI_MSG_MAX];

esp_err_t svc_notification_init(void)
{
    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == NULL) return ESP_ERR_NO_MEM;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

static void noti_lock(void)
{
    if (s_mux != NULL) xSemaphoreTake(s_mux, portMAX_DELAY);
}

static void noti_unlock(void)
{
    if (s_mux != NULL) xSemaphoreGive(s_mux);
}

esp_err_t svc_notification_post(const svc_notification_t *noti)
{
    if (noti == NULL) return ESP_ERR_INVALID_ARG;

    noti_lock();

    int slot = -1;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (!s_slots[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        noti_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_slots[slot].used = true;
    s_slots[slot].noti = *noti;
    s_slots[slot].noti.id = s_next_id++;

    s_slots[slot].title[0] = '\0';
    s_slots[slot].message[0] = '\0';
    if (noti->title) strlcpy(s_slots[slot].title, noti->title, sizeof(s_slots[slot].title));
    if (noti->message) strlcpy(s_slots[slot].message, noti->message, sizeof(s_slots[slot].message));
    s_slots[slot].noti.title = s_slots[slot].title;
    s_slots[slot].noti.message = s_slots[slot].message;
    s_slots[slot].noti.timestamp = noti->timestamp ? noti->timestamp
                                                   : (uint32_t)svc_time_now();

    /* 事件负载用独立副本（字符串指针指向 s_last_*），订阅者拿到的内容不会被后续 dismiss 影响 */
    strlcpy(s_last_title, s_slots[slot].title, sizeof(s_last_title));
    strlcpy(s_last_msg, s_slots[slot].message, sizeof(s_last_msg));
    s_last_posted = s_slots[slot].noti;
    s_last_posted.title = s_last_title;
    s_last_posted.message = s_last_msg;

    noti_unlock();

    svc_event_bus_publish(SVC_EVENT_NOTIFICATION_POSTED, &s_last_posted, sizeof(s_last_posted));
    return ESP_OK;
}

esp_err_t svc_notification_dismiss(uint32_t noti_id)
{
    noti_lock();

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (s_slots[i].used && s_slots[i].noti.id == noti_id) {
            s_slots[i].used = false;
            ret = ESP_OK;
            break;
        }
    }

    noti_unlock();

    if (ret == ESP_OK) {
        svc_event_bus_publish(SVC_EVENT_NOTIFICATION_DISMISSED, &noti_id, sizeof(noti_id));
    }
    return ret;
}

esp_err_t svc_notification_clear_all(void)
{
    noti_lock();

    uint32_t ids[SVC_NOTI_MAX];
    size_t n = 0;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (!s_slots[i].used) continue;
        ids[n++] = s_slots[i].noti.id;
        s_slots[i].used = false;
    }

    noti_unlock();

    for (size_t i = 0; i < n; i++) {
        svc_event_bus_publish(SVC_EVENT_NOTIFICATION_DISMISSED, &ids[i], sizeof(ids[i]));
    }
    return ESP_OK;
}

size_t svc_notification_get_count(void)
{
    noti_lock();

    size_t n = 0;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (s_slots[i].used) n++;
    }

    noti_unlock();
    return n;
}

esp_err_t svc_notification_get(size_t index, svc_notification_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    noti_lock();

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    size_t n = 0;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (!s_slots[i].used) continue;
        if (n == index) {
            *out = s_slots[i].noti;
            ret = ESP_OK;
            break;
        }
        n++;
    }

    noti_unlock();
    return ret;
}

esp_err_t svc_notification_find(uint32_t noti_id, svc_notification_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    noti_lock();

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (s_slots[i].used && s_slots[i].noti.id == noti_id) {
            *out = s_slots[i].noti;
            ret = ESP_OK;
            break;
        }
    }

    noti_unlock();
    return ret;
}
