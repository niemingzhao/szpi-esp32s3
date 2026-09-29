/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Notification
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 通知类型
 */
typedef enum {
    SVC_NOTI_TYPE_INFO,
    SVC_NOTI_TYPE_WARN,
    SVC_NOTI_TYPE_ERROR,
    SVC_NOTI_TYPE_SUCCESS,
    SVC_NOTI_TYPE_PROGRESS,
} svc_noti_type_t;

/**
 * @brief 通知点击回调
 */
typedef void (*svc_noti_click_cb_t)(uint32_t noti_id, void *user_data);

/**
 * @brief 通知对象
 */
typedef struct {
    uint32_t id;
    svc_noti_type_t type;
    const char *icon_src;
    const char *title;
    const char *message;
    uint32_t timestamp;
    bool auto_dismiss_ms;
    void *user_data;
    svc_noti_click_cb_t on_click;
} svc_notification_t;

/**
 * @brief 初始化通知服务
 */
esp_err_t svc_notification_init(void);

/**
 * @brief 发布 / 撤销 / 清空通知
 */
esp_err_t svc_notification_post(const svc_notification_t *noti);
esp_err_t svc_notification_dismiss(uint32_t noti_id);
esp_err_t svc_notification_clear_all(void);

/**
 * @brief 当前通知数量
 */
size_t svc_notification_get_count(void);

/**
 * @brief 按顺序读取第 index 条通知（title / message 指向服务内部缓冲区，调用方只读）
 */
esp_err_t svc_notification_get(size_t index, svc_notification_t *out);

/**
 * @brief 按通知 ID 查找
 *
 * 点击列表条目时用这个：列表可能已经重建，只有 ID 是稳定标识。
 */
esp_err_t svc_notification_find(uint32_t noti_id, svc_notification_t *out);

/**
 * @brief 新通知提示音开关（默认开；正在放音时不打断）
 */
esp_err_t svc_notification_set_sound(bool on);

/**
 * @brief 查询提示音开关
 */
bool svc_notification_get_sound(void);

#ifdef __cplusplus
}
#endif
