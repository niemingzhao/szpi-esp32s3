/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IO 外扩接口实现（periph_ext 的薄封装）
 */

#include "svc_common.h"
#include "svc_io.h"
#include "periph_common.h"
#include "esp_log.h"

static const char *TAG = "svc.io";

static bool s_initialized = false;

esp_err_t svc_io_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = periph_ext_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_io_gpio_write(uint8_t gpio, uint8_t level)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_gpio_write(gpio, level);
}

int svc_io_gpio_read(uint8_t gpio)
{
    if (!s_initialized) return -1;
    return periph_ext_gpio_read(gpio);
}

esp_err_t svc_io_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_pwm_set(gpio, freq_hz, duty_percent);
}

esp_err_t svc_io_pwm_stop(uint8_t gpio)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_pwm_stop(gpio);
}

esp_err_t svc_io_adc_read(uint8_t gpio, int *out_mv)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_adc_read(gpio, out_mv);
}

esp_err_t svc_io_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_i2c_write(addr, data, len);
}

esp_err_t svc_io_i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_i2c_read(addr, data, len);
}

esp_err_t svc_io_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_uart_config(baud, data_bits, parity, stop_bits);
}

esp_err_t svc_io_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_uart_write(data, len, timeout_ms);
}

esp_err_t svc_io_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_uart_read(data, len, read_len, timeout_ms);
}
