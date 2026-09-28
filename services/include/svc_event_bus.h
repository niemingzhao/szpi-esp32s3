/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Event Bus
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 事件 ID
 */
typedef enum {
    SVC_EVENT_BASE = 0,

    /* 存储事件 */
    SVC_EVENT_SD_MOUNTED,
    SVC_EVENT_SD_UNMOUNTED,
    SVC_EVENT_SD_ERROR,

    /* 网络事件 */
    SVC_EVENT_WIFI_SCAN_STARTED,
    SVC_EVENT_WIFI_SCAN_DONE,
    SVC_EVENT_WIFI_CONNECTING,
    SVC_EVENT_WIFI_CONNECTED,
    SVC_EVENT_WIFI_DISCONNECTED,
    SVC_EVENT_WIFI_CONNECT_FAILED,

    /* 时间事件 */
    SVC_EVENT_TIME_SYNCED,
    SVC_EVENT_TIME_CHANGED,
    SVC_EVENT_TIMEZONE_CHANGED,

    /* 主题 / 语言事件 */
    SVC_EVENT_THEME_CHANGED,
    SVC_EVENT_LANGUAGE_CHANGED,

    /* 电源事件 */
    SVC_EVENT_BRIGHTNESS_CHANGED,
    SVC_EVENT_TOUCH,
    SVC_EVENT_SHUTDOWN_REQUEST,

    /* 手势事件 */
    SVC_EVENT_GESTURE_SWIPE_LEFT,
    SVC_EVENT_GESTURE_SWIPE_RIGHT,
    SVC_EVENT_GESTURE_SWIPE_UP,
    SVC_EVENT_GESTURE_SWIPE_DOWN,

    /* 音频事件 */
    SVC_EVENT_AUDIO_PLAYBACK_STARTED,
    SVC_EVENT_AUDIO_PLAYBACK_FINISHED,
    SVC_EVENT_AUDIO_PLAYBACK_ERROR,
    SVC_EVENT_AUDIO_RECORD_STARTED,
    SVC_EVENT_AUDIO_RECORD_FINISHED,

    /* 通知事件 */
    SVC_EVENT_NOTIFICATION_POSTED,
    SVC_EVENT_NOTIFICATION_DISMISSED,
    SVC_EVENT_NOTIFICATION_CLICKED,

    /* 用户事件 */
    SVC_EVENT_USER_BASE = 0x8000,
} svc_event_id_t;

/**
 * @brief 事件对象（data 生命周期由发布者 / 分发机制保证）
 */
typedef struct {
    svc_event_id_t id;
    void *data;
    uint32_t data_len;
} svc_event_t;

/**
 * @brief 事件处理回调（在 dispatcher 任务中执行，禁止阻塞过久）
 */
typedef void (*svc_event_handler_t)(const svc_event_t *evt, void *user_data);

/**
 * @brief 初始化事件总线
 */
esp_err_t svc_event_bus_init(void);

/**
 * @brief 订阅事件
 */
esp_err_t svc_event_bus_subscribe(svc_event_id_t id, svc_event_handler_t h, void *user_data);

/**
 * @brief 取消订阅（h 为 NULL 时取消该 id 下 user_data 匹配的订阅）
 */
esp_err_t svc_event_bus_unsubscribe(svc_event_id_t id, svc_event_handler_t h);

/**
 * @brief 发布事件（异步，不阻塞调用者）
 */
esp_err_t svc_event_bus_publish(svc_event_id_t id, void *data, uint32_t len);

/**
 * @brief 从 ISR 发布事件
 */
esp_err_t svc_event_bus_publish_from_isr(svc_event_id_t id, void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
