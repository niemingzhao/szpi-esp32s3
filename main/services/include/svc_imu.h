/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IMU 运动 / 姿态
 *
 * QMI8658 的中断引脚未引出，本服务每 50 ms 轮询一次，自己判定运动 / 朝向 / 摇晃 /
 * 抬手（判定逻辑不放 Drivers / Peripherals 层），成立时发布
 * SVC_EVENT_IMU_MOTION / SVC_EVENT_IMU_ORIENTATION / SVC_EVENT_IMU_SHAKE /
 * SVC_EVENT_IMU_PICKUP。
 *
 * 判定阈值（运动合加速度百分比、角速度、朝向轴占比、摇晃次数与窗口、抬手静止 / 阶跃 /
 * 竖持窗口）可用 svc_settings 的 `imu` 命名空间调整，见实现文件里的 key 表；服务启动时
 * 读一次并缓存，运行期改 NVS 需重启服务（或重启设备）才生效。
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "periph_imu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 朝向
 */
typedef enum {
    SVC_IMU_ORIENTATION_PORTRAIT,
    SVC_IMU_ORIENTATION_LANDSCAPE,
    SVC_IMU_ORIENTATION_PORTRAIT_FLIP,
    SVC_IMU_ORIENTATION_LANDSCAPE_FLIP,
} svc_imu_orientation_t;

/**
 * @brief 启动 IMU 轮询（重复调用安全）
 */
esp_err_t svc_imu_init(void);

/**
 * @brief 当前是否在运动
 */
bool svc_imu_is_moving(void);

/**
 * @brief 当前朝向
 */
svc_imu_orientation_t svc_imu_get_orientation(void);

/**
 * @brief 读一次原始数据（加速度 / 角速度 / 欧拉角，供姿态 App 显示）
 */
esp_err_t svc_imu_read(periph_imu_data_t *out);

#ifdef __cplusplus
}
#endif
