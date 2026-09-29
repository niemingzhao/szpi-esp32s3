/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - I2C0 主机总线（新版 i2c_master 驱动）
 *
 * 取代旧版 driver/i2c.h：总线只创建一次，各芯片驱动通过
 * drv_i2c_device_add() 挂上自己的从设备句柄，再读写寄存器。
 */

#include "drv_common.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "drv.i2c";

#define DRV_I2C_TIMEOUT_MS   1000
#define DRV_I2C_MAX_WRITE    8      /* 寄存器地址 + 最多 7 字节数据 */

static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t drv_i2c_bus_init(void)
{
    if (s_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = DRV_I2C_SDA_GPIO,
        .scl_io_num = DRV_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "initialized (I2C0, SDA GPIO%d, SCL GPIO%d, %d kHz)",
             DRV_I2C_SDA_GPIO, DRV_I2C_SCL_GPIO, DRV_I2C_FREQ_HZ / 1000);
    return ESP_OK;
}

i2c_master_bus_handle_t drv_i2c_bus_handle(void)
{
    return s_bus;
}

esp_err_t drv_i2c_device_add(uint16_t dev_addr, uint32_t scl_speed_hz,
                             i2c_master_dev_handle_t *out_dev)
{
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    if (out_dev == NULL) return ESP_ERR_INVALID_ARG;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = dev_addr,
        .scl_speed_hz = scl_speed_hz,
    };

    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, out_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02X failed: %s", dev_addr, esp_err_to_name(err));
        return err;
    }

    /* 探测一下：没应答时给出明确告警，省得到处查"为什么读不到数据" */
    if (i2c_master_probe(s_bus, dev_addr, 200) != ESP_OK) {
        ESP_LOGW(TAG, "device 0x%02X not responding", dev_addr);
    }
    return ESP_OK;
}

esp_err_t drv_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                            const uint8_t *data, size_t len)
{
    if (dev == NULL) return ESP_ERR_INVALID_STATE;
    if (len > DRV_I2C_MAX_WRITE - 1) return ESP_ERR_INVALID_ARG;
    if (len > 0 && data == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t buf[DRV_I2C_MAX_WRITE];
    buf[0] = reg;
    if (len > 0) {
        memcpy(&buf[1], data, len);
    }

    return i2c_master_transmit(dev, buf, len + 1, DRV_I2C_TIMEOUT_MS);
}

esp_err_t drv_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                           uint8_t *data, size_t len)
{
    if (dev == NULL) return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    return i2c_master_transmit_receive(dev, &reg, 1, data, len, DRV_I2C_TIMEOUT_MS);
}
