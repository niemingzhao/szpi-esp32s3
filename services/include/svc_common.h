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
#include "svc_ota.h"
#include "svc_mqtt.h"
#include "svc_ws.h"
#include "svc_sysinfo.h"
#include "svc_shell.h"
#include "svc_imu.h"
#include "svc_watchdog.h"
#include "svc_bt.h"
#include "svc_bt_hid.h"
#include "svc_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化所有服务
 *
 * 顺序：Watchdog → EventBus → Settings → Storage → BT → Time → Audio → Net →
 *       Power → IMU → Notification → SysInfo → Shell
 *
 * 蓝牙必须早于 Wi-Fi（内部内存最干净时初始化，见 AGENTS 4.6）。
 */
esp_err_t services_init(void);

#ifdef __cplusplus
}
#endif
