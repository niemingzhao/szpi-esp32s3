/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - LEDC 背光 PWM（LCD_BL，GPIO42）
 */

#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "drv.ledc";

#define DRV_LEDC_BL_GPIO    GPIO_NUM_42
#define DRV_LEDC_CH         LEDC_CHANNEL_0
#define DRV_LEDC_TIMER      LEDC_TIMER_0
#define DRV_LEDC_MODE       LEDC_LOW_SPEED_MODE

static bool s_initialized = false;

esp_err_t drv_ledc_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const ledc_channel_config_t channel = {
        .gpio_num = DRV_LEDC_BL_GPIO,
        .speed_mode = DRV_LEDC_MODE,
        .channel = DRV_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = DRV_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,  // 背光电路是反相的
    };

    const ledc_timer_config_t timer = {
        .speed_mode = DRV_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = DRV_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));

    s_initialized = true;
    ESP_LOGI(TAG, "LEDC backlight initialized (GPIO42, 5 kHz, 10-bit)");
    return ESP_OK;
}

esp_err_t drv_ledc_set_brightness(uint8_t percent)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (1023 * percent) / 100;  // 10-bit resolution
    esp_err_t err = ledc_set_duty(DRV_LEDC_MODE, DRV_LEDC_CH, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(DRV_LEDC_MODE, DRV_LEDC_CH);
}
