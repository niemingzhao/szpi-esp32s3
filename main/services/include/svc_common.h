/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 公共头
 * 聚合各服务头文件，并提供整层初始化入口。
 *
 * 命名：svc_<服务>_<功能>
 */

#pragma once

#include "svc_watchdog.h"
#include "svc_identity.h"
#include "svc_event_bus.h"
#include "svc_settings.h"
#include "svc_storage.h"
#include "svc_bt.h"
#include "svc_bt_hid.h"
#include "svc_bt_central.h"
#include "svc_time.h"
#include "svc_audio.h"
#include "svc_net.h"
#include "svc_mqtt.h"
#include "svc_ws.h"
#include "svc_power.h"
#include "svc_imu.h"
#include "svc_io.h"
#include "svc_camera.h"
#include "svc_sysinfo.h"
#include "svc_web.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化所有服务
 *
 * 顺序：Watchdog（只配成告警）→ EventBus → Settings → Storage → BT → Time →
 *       Audio → Net → Power → IMU → IO → Camera → SysInfo → Web
 *
 * 蓝牙必须早于 Wi-Fi：控制器与主机都要内部 RAM，趁内部内存最干净时初始化。
 * 看门狗的超时自动重启由 main.c 在挂载内置 Flash 之后打开。
 */
esp_err_t services_init(void);

#ifdef __cplusplus
}
#endif
