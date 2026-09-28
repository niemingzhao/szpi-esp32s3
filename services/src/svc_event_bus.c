/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Event Bus 实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "svc.event_bus";

#define SVC_EVENT_MAX_SUBS   32
#define SVC_EVENT_QUEUE_LEN  32
#define SVC_EVENT_DATA_MAX   64   /* 事件负载按值拷贝的上限 */

typedef struct {
    bool used;
    svc_event_id_t id;
    svc_event_handler_t handler;
    void *user;
} svc_sub_t;

typedef struct {
    svc_event_id_t id;
    uint32_t data_len;
    uint8_t data[SVC_EVENT_DATA_MAX];
} svc_queued_t;

static svc_sub_t s_subs[SVC_EVENT_MAX_SUBS];
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[3072];

static void event_bus_dispatch_task(void *arg)
{
    (void)arg;
    svc_queued_t qevt;

    while (true) {
        if (xQueueReceive(s_queue, &qevt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        svc_event_t evt = {
            .id = qevt.id,
            .data = (qevt.data_len > 0) ? (void *)qevt.data : NULL,
            .data_len = qevt.data_len,
        };

        for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
            if (s_subs[i].used && s_subs[i].id == qevt.id && s_subs[i].handler != NULL) {
                s_subs[i].handler(&evt, s_subs[i].user);
            }
        }
    }
}

esp_err_t svc_event_bus_init(void)
{
    if (s_task != NULL) return ESP_OK;

    s_queue = xQueueCreate(SVC_EVENT_QUEUE_LEN, sizeof(svc_queued_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    s_task = xTaskCreateStaticPinnedToCore(
        event_bus_dispatch_task, "svc_event_bus",
        sizeof(s_task_stack) / sizeof(StackType_t),
        NULL, 5, s_task_stack, &s_task_buffer, 0);
    if (s_task == NULL) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_event_bus_subscribe(svc_event_id_t id, svc_event_handler_t h, void *user_data)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
        if (!s_subs[i].used) {
            s_subs[i].used = true;
            s_subs[i].id = id;
            s_subs[i].handler = h;
            s_subs[i].user = user_data;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t svc_event_bus_unsubscribe(svc_event_id_t id, svc_event_handler_t h)
{
    for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].id == id &&
            (h == NULL || s_subs[i].handler == h)) {
            s_subs[i].used = false;
            if (h != NULL) return ESP_OK;
        }
    }
    return ESP_OK;
}

static esp_err_t event_bus_post(svc_event_id_t id, const void *data, uint32_t len, bool from_isr)
{
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;

    svc_queued_t qevt;
    memset(&qevt, 0, sizeof(qevt));
    qevt.id = id;

    if (data != NULL && len > 0) {
        uint32_t n = (len > SVC_EVENT_DATA_MAX) ? SVC_EVENT_DATA_MAX : len;
        memcpy(qevt.data, data, n);
        qevt.data_len = n;
    }

    if (from_isr) {
        BaseType_t hp = pdFALSE;
        xQueueSendFromISR(s_queue, &qevt, &hp);
        if (hp) portYIELD_FROM_ISR();
    } else {
        xQueueSend(s_queue, &qevt, 0);
    }
    return ESP_OK;
}

esp_err_t svc_event_bus_publish(svc_event_id_t id, void *data, uint32_t len)
{
    return event_bus_post(id, data, len, false);
}

esp_err_t svc_event_bus_publish_from_isr(svc_event_id_t id, void *data, uint32_t len)
{
    return event_bus_post(id, data, len, true);
}
