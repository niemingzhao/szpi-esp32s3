/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IMU
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
    float roll, pitch, yaw;        // 角度
} periph_imu_data_t;

/**
 * @brief 运动状态
 */
typedef enum {
    PERIPH_IMU_MOTION_NONE = 0,
    PERIPH_IMU_MOTION_ANY = 0x20,
    PERIPH_IMU_MOTION_NO = 0x40,
    PERIPH_IMU_MOTION_SIGNIFICANT = 0x80,
} periph_imu_motion_t;

/**
 * @brief 朝向
 */
typedef enum {
    PERIPH_IMU_ORIENTATION_PORTRAIT,
    PERIPH_IMU_ORIENTATION_LANDSCAPE,
    PERIPH_IMU_ORIENTATION_PORTRAIT_FLIP,
    PERIPH_IMU_ORIENTATION_LANDSCAPE_FLIP,
} periph_imu_orientation_t;

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

/**
 * @brief 获取当前运动状态
 */
periph_imu_motion_t periph_imu_get_motion(void);

/**
 * @brief 获取当前朝向
 */
periph_imu_orientation_t periph_imu_get_orientation(void);

#ifdef __cplusplus
}
#endif
