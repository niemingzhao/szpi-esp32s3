/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IMU 运动 / 姿态
 */

#pragma once

#include "esp_err.h"
#include "periph_imu.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 IMU 轮询（重复调用安全）
 *
 * 状态变化时发布 SVC_EVENT_IMU_MOTION / SVC_EVENT_IMU_ORIENTATION。
 */
esp_err_t svc_imu_init(void);

/**
 * @brief 当前是否在运动
 */
bool svc_imu_is_moving(void);

/**
 * @brief 当前朝向
 */
periph_imu_orientation_t svc_imu_get_orientation(void);

/**
 * @brief 读一次原始数据（加速度 / 角速度 / 欧拉角，供姿态 App 显示）
 */
esp_err_t svc_imu_read(periph_imu_data_t *out);

#ifdef __cplusplus
}
#endif
