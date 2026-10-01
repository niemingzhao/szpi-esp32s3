/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IMU
 *
 * 只提供原始数据与欧拉角；运动 / 摇晃 / 姿态的判定在 svc_imu（QMI8658 的中断
 * 引脚未引出，由服务层软件轮询判定）。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IMU 数据
 */
typedef struct {
    float acc_x, acc_y, acc_z;     // m/s²
    float gyr_x, gyr_y, gyr_z;     // °/s
    float roll, pitch, yaw;        // 由加速度推算的角度（yaw 恒为 0）
} periph_imu_data_t;

/**
 * @brief 初始化 IMU（QMI8658）
 */
esp_err_t periph_imu_init(void);

/**
 * @brief 反初始化
 */
esp_err_t periph_imu_deinit(void);

/**
 * @brief 读取 IMU 数据（含由加速度计算的欧拉角）
 */
esp_err_t periph_imu_read(periph_imu_data_t *out);

#ifdef __cplusplus
}
#endif
