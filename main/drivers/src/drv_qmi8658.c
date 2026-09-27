/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - QMI8658 六轴 IMU（I2C 0x6A）
 *
 * I2C @ 0x6A，ACC ±4g 250 Hz，GYR ±512 dps 250 Hz。
 * 寄存器映射与初始化序列见 QMI8658 数据手册。
 *
 * 数据寄存器布局（连续读取）：
 *   0x35 AX_L, 0x36 AX_H, 0x37 AY_L, 0x38 AY_H, 0x39 AZ_L, 0x3A AZ_H,
 *   0x3B GX_L, 0x3C GX_H, 0x3D GY_L, 0x3E GY_H, 0x3F GZ_L, 0x40 GZ_H
 */

#include "drv_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "drv.qmi8658";

#define DRV_QMI8658_ADDR            0x6A
#define DRV_QMI8658_WHO_AM_I_VALUE  0x05

/* 寄存器地址 */
#define DRV_QMI8658_REG_WHO_AM_I    0x00
#define DRV_QMI8658_REG_CTRL1       0x02  /* 地址自动递增等 */
#define DRV_QMI8658_REG_CTRL2       0x03  /* 加速度计配置 */
#define DRV_QMI8658_REG_CTRL3       0x04  /* 陀螺仪配置 */
#define DRV_QMI8658_REG_CTRL7       0x08  /* 加速度/陀螺仪使能 */
#define DRV_QMI8658_REG_AX_L        0x35  /* 加速度起始；连续 12 字节含陀螺仪 */
#define DRV_QMI8658_REG_RESET       0x60

/* 配置值（位域见数据手册 CTRL2 / CTRL3：bit7 为自检位，必须为 0） */
#define DRV_QMI8658_CTRL1_ADDR_INC          0x40
#define DRV_QMI8658_CTRL2_ACC_4G_250HZ      0x15  /* aST=0, aFS=001(±4g), aODR=0101 */
#define DRV_QMI8658_CTRL3_GYR_512DPS_250HZ  0x55  /* gST=0, gFS=101(±512 dps), gODR=0101 */
#define DRV_QMI8658_CTRL7_ACC_GYR_EN        0x03
#define DRV_QMI8658_RESET_CMD               0xB0

/* 数据换算：±4g -> 8192 LSB/g（1<<13）；±512 dps -> 64 LSB/(°/s)（1<<6） */
#define DRV_QMI8658_ACC_LSB_PER_G    8192.0f
#define DRV_QMI8658_GYR_LSB_PER_DPS  64.0f
#define DRV_QMI8658_GRAVITY          9.80665f

static bool s_initialized = false;
static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t qmi8658_write_reg(uint8_t reg, uint8_t data)
{
    return drv_i2c_write_reg(s_dev, reg, &data, 1);
}

static esp_err_t qmi8658_read_buf(uint8_t reg, uint8_t *data, size_t len)
{
    return drv_i2c_read_reg(s_dev, reg, data, len);
}

/* 失败时把设备从总线上摘掉，避免重复 init 时重复挂设备 */
static void qmi8658_release_device(void)
{
    if (s_dev != NULL) {
        drv_i2c_device_remove(s_dev);
        s_dev = NULL;
    }
}

/* 软复位后等待，再按手册顺序配置，最后使能 */
static esp_err_t qmi8658_configure(void)
{
    esp_err_t err = qmi8658_write_reg(DRV_QMI8658_REG_RESET, DRV_QMI8658_RESET_CMD);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));

    err = qmi8658_write_reg(DRV_QMI8658_REG_CTRL1, DRV_QMI8658_CTRL1_ADDR_INC);
    if (err != ESP_OK) return err;
    err = qmi8658_write_reg(DRV_QMI8658_REG_CTRL2, DRV_QMI8658_CTRL2_ACC_4G_250HZ);
    if (err != ESP_OK) return err;
    err = qmi8658_write_reg(DRV_QMI8658_REG_CTRL3, DRV_QMI8658_CTRL3_GYR_512DPS_250HZ);
    if (err != ESP_OK) return err;
    return qmi8658_write_reg(DRV_QMI8658_REG_CTRL7, DRV_QMI8658_CTRL7_ACC_GYR_EN);
}

esp_err_t drv_qmi8658_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t derr = drv_i2c_device_add(DRV_QMI8658_ADDR, DRV_I2C_FREQ_HZ, &s_dev);
    if (derr != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 add device failed");
        return derr;
    }

    uint8_t whoami = 0;
    if (qmi8658_read_buf(DRV_QMI8658_REG_WHO_AM_I, &whoami, 1) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 not responding");
        qmi8658_release_device();
        return ESP_FAIL;
    }
    if (whoami != DRV_QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "QMI8658 unexpected WHO_AM_I: 0x%02x", whoami);
        qmi8658_release_device();
        return ESP_FAIL;
    }

    esp_err_t err = qmi8658_configure();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed: %s", esp_err_to_name(err));
        qmi8658_release_device();
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "QMI8658 initialized (I2C @ 0x%02x, ACC ±4g/GYR ±512 dps)", DRV_QMI8658_ADDR);
    return ESP_OK;
}

esp_err_t drv_qmi8658_read(float *out_acc_x, float *out_acc_y, float *out_acc_z,
                           float *out_gyr_x, float *out_gyr_y, float *out_gyr_z)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_acc_x == NULL || out_acc_y == NULL || out_acc_z == NULL ||
        out_gyr_x == NULL || out_gyr_y == NULL || out_gyr_z == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 一次性读取 12 字节：加速度 6 字节 + 陀螺仪 6 字节（小端） */
    uint8_t raw[12];
    esp_err_t err = qmi8658_read_buf(DRV_QMI8658_REG_AX_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw_acc_x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t raw_acc_y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t raw_acc_z = (int16_t)((raw[5] << 8) | raw[4]);
    int16_t raw_gyr_x = (int16_t)((raw[7] << 8) | raw[6]);
    int16_t raw_gyr_y = (int16_t)((raw[9] << 8) | raw[8]);
    int16_t raw_gyr_z = (int16_t)((raw[11] << 8) | raw[10]);

    *out_acc_x = raw_acc_x / DRV_QMI8658_ACC_LSB_PER_G * DRV_QMI8658_GRAVITY;
    *out_acc_y = raw_acc_y / DRV_QMI8658_ACC_LSB_PER_G * DRV_QMI8658_GRAVITY;
    *out_acc_z = raw_acc_z / DRV_QMI8658_ACC_LSB_PER_G * DRV_QMI8658_GRAVITY;

    *out_gyr_x = raw_gyr_x / DRV_QMI8658_GYR_LSB_PER_DPS;
    *out_gyr_y = raw_gyr_y / DRV_QMI8658_GYR_LSB_PER_DPS;
    *out_gyr_z = raw_gyr_z / DRV_QMI8658_GYR_LSB_PER_DPS;

    return ESP_OK;
}
