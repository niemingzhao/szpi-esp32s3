/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - IMU (wraps QMI8658 driver)
 *
 * 只做驱动适配与欧拉角换算；运动 / 姿态的判定在 svc_imu。
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "periph.imu";

static bool s_initialized = false;

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
    return ESP_OK;
}
