/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IMU (wraps QMI8658 driver)
 *
 * 运动 / 姿态判定放在这里：QMI8658 的中断引脚未引出，只能软件判定。
 * 状态由最近一次 periph_imu_read() 更新，periph_imu_get_motion() /
 * periph_imu_get_orientation() 直接返回缓存值，不再访问 I2C。
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "periph.imu";

/* 判定阈值 */
#define IMU_GRAVITY_MS2        9.80665f
#define IMU_MOTION_ACC_RATIO   0.15f    /* 合加速度偏离 1 g 超过 15% 视为运动 */
#define IMU_MOTION_GYRO_DPS    60.0f    /* 角速度超过 60 °/s 视为运动 */
#define IMU_ORIENT_AXIS_G      0.60f    /* 重力在水平轴上的投影占比，用于判朝向 */

static bool s_initialized = false;
static volatile periph_imu_motion_t s_motion = PERIPH_IMU_MOTION_NONE;
static volatile periph_imu_orientation_t s_orientation = PERIPH_IMU_ORIENTATION_PORTRAIT;

esp_err_t periph_imu_init(void)
{
    if (s_initialized) return ESP_OK;
    esp_err_t err = drv_qmi8658_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed");
        return err;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t periph_imu_deinit(void)
{
    if (!s_initialized) return ESP_OK;
    drv_qmi8658_deinit();
    s_initialized = false;
    s_motion = PERIPH_IMU_MOTION_NONE;
    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

esp_err_t periph_imu_read(periph_imu_data_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t err = drv_qmi8658_read(&out->acc_x, &out->acc_y, &out->acc_z,
                                     &out->gyr_x, &out->gyr_y, &out->gyr_z);
    if (err != ESP_OK) return err;

    /* 由加速度计算倾角（简化算法，yaw 恒为 0） */
    const float rad2deg = 57.29578f;
    out->roll = atan2f(out->acc_y, out->acc_z) * rad2deg;
    out->pitch = atan2f(-out->acc_x,
                        sqrtf(out->acc_y * out->acc_y + out->acc_z * out->acc_z)) * rad2deg;
    out->yaw = 0.0f;

    /* 运动：合加速度偏离 1 g，或角速度偏大 */
    float acc_mag = sqrtf(out->acc_x * out->acc_x + out->acc_y * out->acc_y +
                          out->acc_z * out->acc_z);
    float gyr_mag = sqrtf(out->gyr_x * out->gyr_x + out->gyr_y * out->gyr_y +
                          out->gyr_z * out->gyr_z);
    bool moving = (fabsf(acc_mag - IMU_GRAVITY_MS2) > IMU_MOTION_ACC_RATIO * IMU_GRAVITY_MS2) ||
                  (gyr_mag > IMU_MOTION_GYRO_DPS);
    s_motion = moving ? PERIPH_IMU_MOTION_SIGNIFICANT : PERIPH_IMU_MOTION_NONE;

    /* 姿态：看重力主要落在哪个水平轴上（屏幕平放时 z 占优，保持上一次结果） */
    if (acc_mag > 0.01f) {
        float nx = fabsf(out->acc_x) / acc_mag;
        float ny = fabsf(out->acc_y) / acc_mag;
        float nz = fabsf(out->acc_z) / acc_mag;

        if (nz < IMU_ORIENT_AXIS_G) {
            if (ny >= nx) {
                s_orientation = (out->acc_y >= 0.0f) ? PERIPH_IMU_ORIENTATION_PORTRAIT
                                                     : PERIPH_IMU_ORIENTATION_PORTRAIT_FLIP;
            } else {
                s_orientation = (out->acc_x >= 0.0f) ? PERIPH_IMU_ORIENTATION_LANDSCAPE
                                                     : PERIPH_IMU_ORIENTATION_LANDSCAPE_FLIP;
            }
        }
    }
    return ESP_OK;
}

periph_imu_motion_t periph_imu_get_motion(void)
{
    return s_motion;
}

periph_imu_orientation_t periph_imu_get_orientation(void)
{
    return s_orientation;
}
