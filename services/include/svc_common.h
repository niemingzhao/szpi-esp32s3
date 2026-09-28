/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Common
 * 聚合各服务头文件，并提供整层初始化入口。
 *
 * 命名：svc_<服务>_<功能>
 */

#pragma once

#include "svc_event_bus.h"
#include "svc_settings.h"
#include "svc_storage.h"
#include "svc_time.h"
#include "svc_power.h"
#include "svc_notification.h"
#include "svc_audio.h"
#include "svc_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化所有服务
 *
 * 顺序：EventBus → Settings → Storage → Time → Audio → Net → Power → Notification
 */
esp_err_t services_init(void);

#ifdef __cplusplus
}
#endif
