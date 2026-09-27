/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IO 外扩接口实现（periph_ext 的薄封装）
 */

#include "svc_common.h"
#include "svc_io.h"
#include "periph_common.h"
#include "esp_log.h"
#include <string.h>

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

esp_err_t svc_io_can_config(uint32_t bitrate, bool listen_only)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_can_config(bitrate, listen_only);
}

esp_err_t svc_io_can_stop(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return periph_ext_can_stop();
}

esp_err_t svc_io_can_send(const svc_io_can_frame_t *frame, uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (frame == NULL) return ESP_ERR_INVALID_ARG;

    /* Services 与 Peripherals 各一套帧类型，逐字段转一次 */
    periph_ext_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id = frame->id;
    f.extended = frame->extended;
    f.len = frame->len;
    memcpy(f.data, frame->data, sizeof(f.data));
    return periph_ext_can_send(&f, timeout_ms);
}

esp_err_t svc_io_can_receive(svc_io_can_frame_t *out, uint32_t timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    periph_ext_can_frame_t f;
    esp_err_t err = periph_ext_can_receive(&f, timeout_ms);
    if (err != ESP_OK) return err;

    memset(out, 0, sizeof(*out));
    out->id = f.id;
    out->extended = f.extended;
    out->len = f.len;
    memcpy(out->data, f.data, sizeof(out->data));
    return ESP_OK;
}
