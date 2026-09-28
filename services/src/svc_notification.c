/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Notification 实现
 */

#include "svc_common.h"
#include "esp_log.h"
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

esp_err_t svc_notification_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_notification_post(const svc_notification_t *noti)
{
    if (noti == NULL) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (s_slots[i].used) continue;

        s_slots[i].used = true;
        s_slots[i].noti = *noti;
        s_slots[i].noti.id = s_next_id++;

        s_slots[i].title[0] = '\0';
        s_slots[i].message[0] = '\0';
        if (noti->title) strlcpy(s_slots[i].title, noti->title, sizeof(s_slots[i].title));
        if (noti->message) strlcpy(s_slots[i].message, noti->message, sizeof(s_slots[i].message));
        s_slots[i].noti.title = s_slots[i].title;
        s_slots[i].noti.message = s_slots[i].message;
        s_slots[i].noti.timestamp = noti->timestamp ? noti->timestamp
                                                    : (uint32_t)svc_time_now();

        svc_event_bus_publish(SVC_EVENT_NOTIFICATION_POSTED, &s_slots[i].noti,
                              sizeof(s_slots[i].noti));
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t svc_notification_dismiss(uint32_t noti_id)
{
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (s_slots[i].used && s_slots[i].noti.id == noti_id) {
            s_slots[i].used = false;
            svc_event_bus_publish(SVC_EVENT_NOTIFICATION_DISMISSED, &noti_id, sizeof(noti_id));
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t svc_notification_clear_all(void)
{
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (!s_slots[i].used) continue;
        uint32_t id = s_slots[i].noti.id;
        s_slots[i].used = false;
        svc_event_bus_publish(SVC_EVENT_NOTIFICATION_DISMISSED, &id, sizeof(id));
    }
    return ESP_OK;
}

size_t svc_notification_get_count(void)
{
    size_t n = 0;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (s_slots[i].used) n++;
    }
    return n;
}

esp_err_t svc_notification_get(size_t index, svc_notification_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    size_t n = 0;
    for (int i = 0; i < SVC_NOTI_MAX; i++) {
        if (!s_slots[i].used) continue;
        if (n == index) {
            *out = s_slots[i].noti;
            return ESP_OK;
        }
        n++;
    }
    return ESP_ERR_NOT_FOUND;
}
