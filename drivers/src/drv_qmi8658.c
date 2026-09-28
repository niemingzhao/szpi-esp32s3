/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - QMI8658 IMU Driver
 *
 * I2C @ 0x6A, ACC ±4g 250Hz, GYR ±512dps 250Hz
 */

#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "drv.qmi8658";

#define QMI8658_ADDR          0x6A

// QMI8658 寄存器定义 (根据 QMI8658 datasheet)
#define QMI8658_REG_WHO_AM_I     0x00
#define QMI8658_WHO_AM_I_VAL     0x05

#define QMI8658_REG_PWR_CTRL     0x02  // Power control
#define QMI8658_REG_PWR_CTRL_ACC BIT(0)
#define QMI8658_REG_PWR_CTRL_GYR BIT(1)

#define QMI8658_REG_CTRL1        0x03  // Accelerometer control
#define QMI8658_REG_CTRL2        0x04  // Gyroscope control
#define QMI8658_REG_CTRL3        0x05  // Data selection

// ACC: ±4g (0x2C), 250Hz ODR (0x09)
// GYR: ±512dps (0x23), 250Hz ODR (0x09)

static bool s_initialized = false;

static esp_err_t qmi8658_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    return i2c_master_write_to_device(0, QMI8658_ADDR, buf, sizeof(buf), 1000 / portTICK_PERIOD_MS);
}

static esp_err_t qmi8658_read_reg(uint8_t reg, uint8_t *data)
{
    return i2c_master_write_read_device(0, QMI8658_ADDR, &reg, 1, data, 1, 1000 / portTICK_PERIOD_MS);
}

esp_err_t drv_qmi8658_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    uint8_t whoami = 0;
    if (qmi8658_read_reg(QMI8658_REG_WHO_AM_I, &whoami) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 not responding");
        return ESP_FAIL;
    }

    if (whoami != QMI8658_WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "QMI8658 unexpected WHO_AM_I: 0x%02x", whoami);
        return ESP_FAIL;
    }

    // 启动 ACC + GYR
    ESP_ERROR_CHECK(qmi8658_write_reg(QMI8658_REG_PWR_CTRL,
                                       QMI8658_REG_PWR_CTRL_ACC | QMI8658_REG_PWR_CTRL_GYR));

    // 配置 ACC: ±4g (0x2C), 250Hz (0x09)
    ESP_ERROR_CHECK(qmi8658_write_reg(QMI8658_REG_CTRL1, 0x2C));  // ACC config
    ESP_ERROR_CHECK(qmi8658_write_reg(0x09, 0x0B));               // ODR

    // 配置 GYR: ±512dps (0x23), 250Hz (0x09)
    ESP_ERROR_CHECK(qmi8658_write_reg(QMI8658_REG_CTRL2, 0x23));  // GYR config
    ESP_ERROR_CHECK(qmi8658_write_reg(0x0A, 0x0B));               // ODR

    s_initialized = true;
    ESP_LOGI(TAG, "QMI8658 initialized (I2C @ 0x%02x)", QMI8658_ADDR);
    return ESP_OK;
}

esp_err_t drv_qmi8658_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    // 关闭传感器
    qmi8658_write_reg(QMI8658_REG_PWR_CTRL, 0x00);
    s_initialized = false;
    ESP_LOGI(TAG, "QMI8658 deinitialized");
    return ESP_OK;
}

esp_err_t drv_qmi8658_read(float *out_acc_x, float *out_acc_y, float *out_acc_z,
                           float *out_gyr_x, float *out_gyr_y, float *out_gyr_z)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // 读取加速度原始数据 (0x35-0x37, 3 axes × 2 bytes, little-endian)
    uint8_t acc_data[6];
    if (qmi8658_read_reg(0x35, &acc_data[0]) != ESP_OK ||
        qmi8658_read_reg(0x36, &acc_data[1]) != ESP_OK ||
        qmi8658_read_reg(0x37, &acc_data[2]) != ESP_OK) {
        return ESP_FAIL;
    }

    // 读取角速度原始数据 (0x38-0x3A, 3 axes × 2 bytes, little-endian)
    uint8_t gyr_data[6];
    if (qmi8658_read_reg(0x38, &gyr_data[0]) != ESP_OK ||
        qmi8658_read_reg(0x39, &gyr_data[1]) != ESP_OK ||
        qmi8658_read_reg(0x3A, &gyr_data[2]) != ESP_OK) {
        return ESP_FAIL;
    }

    int16_t raw_acc_x = (int16_t)(acc_data[1] << 8) | acc_data[0];
    int16_t raw_acc_y = (int16_t)(acc_data[3] << 8) | acc_data[2];
    int16_t raw_acc_z = (int16_t)(acc_data[5] << 8) | acc_data[4];

    int16_t raw_gyr_x = (int16_t)(gyr_data[1] << 8) | gyr_data[0];
    int16_t raw_gyr_y = (int16_t)(gyr_data[3] << 8) | gyr_data[2];
    int16_t raw_gyr_z = (int16_t)(gyr_data[5] << 8) | gyr_data[4];

    // 转换: ±4g → LSB = 4096 LSB/g, ±512dps → 16 LSB/(°/s)
    // 注: 实际转换因子需根据 datasheet 微调
    *out_acc_x = raw_acc_x / 4096.0f * 9.81f;
    *out_acc_y = raw_acc_y / 4096.0f * 9.81f;
    *out_acc_z = raw_acc_z / 4096.0f * 9.81f;

    *out_gyr_x = raw_gyr_x / 16.0f;
    *out_gyr_y = raw_gyr_y / 16.0f;
    *out_gyr_z = raw_gyr_z / 16.0f;

    return ESP_OK;
}
