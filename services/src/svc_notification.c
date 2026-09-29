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

/* 提示音：默认开，可用 svc_notification_set_sound() 关闭 */
#define NOTI_TONE_HZ   1000
#define NOTI_TONE_MS   100
static bool s_sound = true;

/* ------------------------------- 持久化 ------------------------------- */

#define NOTI_PERSIST_NS     "sys"
#define NOTI_PERSIST_KEY    "noti_log"
#define NOTI_PERSIST_MAGIC  0x4E4F5449u   /* "NOTI" */
#define NOTI_PERSIST_MAX    8             /* 只存最近 8 条，控制 NVS 占用 */

typedef struct {
    uint32_t id;
    uint32_t timestamp;
    uint8_t type;
    char title[SVC_NOTI_TITLE_MAX];
    char message[SVC_NOTI_MSG_MAX];
} noti_persist_item_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    noti_persist_item_t items[NOTI_PERSIST_MAX];
} noti_persist_t;

static noti_persist_t s_persist;

/* 持锁调用：把当前槽位写进 NVS（回调指针无法持久化，恢复后点击不跳转） */
static void noti_save_locked(void)
{
    s_persist.magic = NOTI_PERSIST_MAGIC;
    s_persist.count = 0;

    for (int i = 0; i < SVC_NOTI_MAX && s_persist.count < NOTI_PERSIST_MAX; i++) {
        if (!s_slots[i].used) continue;

        noti_persist_item_t *it = &s_persist.items[s_persist.count];
        memset(it, 0, sizeof(*it));
        it->id = s_slots[i].noti.id;
        it->timestamp = s_slots[i].noti.timestamp;
        it->type = (uint8_t)s_slots[i].noti.type;
        strlcpy(it->title, s_slots[i].title, sizeof(it->title));
        strlcpy(it->message, s_slots[i].message, sizeof(it->message));
        s_persist.count++;
    }

    svc_settings_set_blob(NOTI_PERSIST_NS, NOTI_PERSIST_KEY, &s_persist, sizeof(s_persist));
}

/* 持锁调用：从 NVS 恢复（on_click / user_data 不恢复） */
static void noti_load_locked(void)
{
    size_t len = sizeof(s_persist);
    memset(&s_persist, 0, sizeof(s_persist));

    if (svc_settings_get_blob(NOTI_PERSIST_NS, NOTI_PERSIST_KEY, &s_persist, &len) != ESP_OK) return;
    if (s_persist.magic != NOTI_PERSIST_MAGIC) return;
    if (s_persist.count > NOTI_PERSIST_MAX) return;

    uint32_t max_id = 0;
    for (uint32_t i = 0; i < s_persist.count && i < SVC_NOTI_MAX; i++) {
        const noti_persist_item_t *it = &s_persist.items[i];
        svc_noti_slot_t *slot = &s_slots[i];

        slot->used = true;
        memset(&slot->noti, 0, sizeof(slot->noti));
        slot->noti.id = it->id;
        slot->noti.type = (svc_noti_type_t)it->type;
        slot->noti.timestamp = it->timestamp;
        strlcpy(slot->title, it->title, sizeof(slot->title));
        strlcpy(slot->message, it->message, sizeof(slot->message));
        slot->noti.title = slot->title;
        slot->noti.message = slot->message;
        slot->noti.on_click = NULL;
        slot->noti.user_data = NULL;

        if (it->id > max_id) max_id = it->id;
    }

    if (max_id + 1 > s_next_id) s_next_id = max_id + 1;
}

esp_err_t svc_notification_init(void)
{
    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == NULL) return ESP_ERR_NO_MEM;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;
    noti_load_locked();          /* 恢复上次运行的通知（回调指针无法恢复） */
    ESP_LOGI(TAG, "initialized (%u restored)", (unsigned)svc_notification_get_count());
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

    /* 提示音：正在放音时不打断（单条 I2S 通路无法混音），其余情况播一声短提示 */
    if (s_sound && svc_audio_get_state() == SVC_AUDIO_STATE_IDLE) {
        svc_audio_play_tone_async(NOTI_TONE_HZ, NOTI_TONE_MS);
    }

    noti_lock();
    noti_save_locked();
    noti_unlock();
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

        noti_lock();
        noti_save_locked();
        noti_unlock();
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

    noti_lock();
    noti_save_locked();
    noti_unlock();
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

esp_err_t svc_notification_set_sound(bool on)
{
    s_sound = on;
    return ESP_OK;
}

bool svc_notification_get_sound(void)
{
    return s_sound;
}
