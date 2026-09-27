/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 事件总线实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "svc.event_bus";

#define SVC_EVENT_MAX_SUBS   32
#define SVC_EVENT_QUEUE_LEN  32

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
static SemaphoreHandle_t s_mux = NULL;   /* 保护 s_subs（订阅可来自任意任务） */
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static StaticTask_t s_task_buffer;
/* 订阅回调在这个任务里跑，而回调可能建界面（App 刷新列表、配对浮层）。
 * 3072 字节实测会被压爆，这里给足余量；新订阅者仍应只做轻活、
 * 深调用（建控件）用 lvgl_port_lock + lv_async_call 交回 LVGL 任务 */
static StackType_t s_task_stack[6144];

static void event_bus_dispatch_task(void *arg)
{
    (void)arg;
    svc_queued_t qevt;

    /* 纳入 Task WDT：总线卡死时要能被看门狗发现 */
    if (svc_watchdog_subscribe() != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog subscribe failed");
    }

    while (true) {
        /* 有限等待而非 portMAX_DELAY：空闲时也能周期喂狗 */
        bool got = (xQueueReceive(s_queue, &qevt, pdMS_TO_TICKS(1000)) == pdTRUE);
        svc_watchdog_feed();
        if (!got) {
            continue;
        }

        svc_event_t evt = {
            .id = qevt.id,
            .data = (qevt.data_len > 0) ? (void *)qevt.data : NULL,
            .data_len = qevt.data_len,
        };

        /* 先持锁取一份订阅快照，再在锁外回调：这样回调里退订/订阅既不会死锁，
         * 也不会因为遍历中途被改而漏发 */
        svc_event_handler_t handlers[SVC_EVENT_MAX_SUBS];
        void *users[SVC_EVENT_MAX_SUBS];
        int n = 0;

        if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
                if (s_subs[i].used && s_subs[i].id == qevt.id && s_subs[i].handler != NULL) {
                    handlers[n] = s_subs[i].handler;
                    users[n] = s_subs[i].user;
                    n++;
                }
            }
            xSemaphoreGive(s_mux);
        }

        for (int i = 0; i < n; i++) {
            handlers[i](&evt, users[i]);
        }
    }
}

esp_err_t svc_event_bus_init(void)
{
    if (s_task != NULL) return ESP_OK;

    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == NULL) return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreate(SVC_EVENT_QUEUE_LEN, sizeof(svc_queued_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    s_task = xTaskCreateStaticPinnedToCore(
        event_bus_dispatch_task, "event_bus_task",
        sizeof(s_task_stack) / sizeof(StackType_t),
        NULL, 4, s_task_stack, &s_task_buffer, 0);
    if (s_task == NULL) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_event_bus_subscribe(svc_event_id_t id, svc_event_handler_t h, void *user_data)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_ERR_NO_MEM;

    /* 同一 (id, handler, user_data) 只登记一次，避免重复回调 */
    for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].id == id &&
            s_subs[i].handler == h && s_subs[i].user == user_data) {
            ret = ESP_OK;
            break;
        }
    }

    if (ret != ESP_OK) {
        for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
            if (!s_subs[i].used) {
                s_subs[i].used = true;
                s_subs[i].id = id;
                s_subs[i].handler = h;
                s_subs[i].user = user_data;
                ret = ESP_OK;
                break;
            }
        }
    }

    xSemaphoreGive(s_mux);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "subscription table full (%d)", SVC_EVENT_MAX_SUBS);
    }
    return ret;
}

esp_err_t svc_event_bus_unsubscribe(svc_event_id_t id, svc_event_handler_t h)
{
    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < SVC_EVENT_MAX_SUBS; i++) {
        if (s_subs[i].used && s_subs[i].id == id &&
            (h == NULL || s_subs[i].handler == h)) {
            s_subs[i].used = false;
            if (h != NULL) break;          /* 指定 handler 时只删掉第一条 */
        }
    }

    xSemaphoreGive(s_mux);
    return ESP_OK;
}

static esp_err_t event_bus_post(svc_event_id_t id, const void *data, uint32_t len)
{
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;

    svc_queued_t qevt;
    memset(&qevt, 0, sizeof(qevt));
    qevt.id = id;

    if (data != NULL && len > 0) {
        uint32_t n = (len > SVC_EVENT_DATA_MAX) ? SVC_EVENT_DATA_MAX : len;
        if (n < len) {
            ESP_LOGW(TAG, "event %d payload truncated: %u -> %u",
                     (int)id, (unsigned)len, (unsigned)n);
        }
        memcpy(qevt.data, data, n);
        qevt.data_len = n;
    }

    if (xQueueSend(s_queue, &qevt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, event %d dropped", (int)id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t svc_event_bus_publish(svc_event_id_t id, const void *data, uint32_t len)
{
    return event_bus_post(id, data, len);
}
