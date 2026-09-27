/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 外设层初始化入口
 */

#include "periph_common.h"
#include "esp_log.h"

static const char *TAG = "periph.init";

esp_err_t peripherals_init_all(void)
{
    ESP_LOGI(TAG, "=== peripherals init start ===");

    // 1. IO 扩展（最早，PA_EN / LCD_CS 依赖它）
    ESP_ERROR_CHECK(periph_io_exp_init());
    ESP_LOGI(TAG, "io_exp initialized");

    // 2. 音频
    if (periph_audio_init() == ESP_OK) {
        ESP_LOGI(TAG, "audio initialized");
    } else {
        ESP_LOGW(TAG, "audio init failed, continuing without audio");
    }

    // 3. LCD
    ESP_ERROR_CHECK(periph_lcd_init());
    ESP_LOGI(TAG, "lcd initialized");

    // 4. 触摸
    ESP_ERROR_CHECK(periph_touch_init());
    ESP_LOGI(TAG, "touch initialized");

    // 5. IMU
    if (periph_imu_init() == ESP_OK) {
        ESP_LOGI(TAG, "imu initialized");
    } else {
        ESP_LOGW(TAG, "imu init failed, continuing without imu");
    }

    // 6. 存储（TF 卡在此挂载；内置 Flash 由 app_main 在 UI 之后挂载）
    if (periph_storage_init() == ESP_OK) {
        ESP_LOGI(TAG, "storage initialized");
        periph_storage_mount(PERIPH_STORAGE_TF_CARD);       // 无卡时仅告警
    } else {
        ESP_LOGW(TAG, "storage init failed");
    }

    // 7. 按键
    ESP_ERROR_CHECK(periph_button_init());
    ESP_LOGI(TAG, "button initialized");

    // 8. 外扩接口（失败不阻塞启动）
    if (periph_ext_init() == ESP_OK) {
        ESP_LOGI(TAG, "ext initialized");
    } else {
        ESP_LOGW(TAG, "ext init failed, continuing without ext");
    }

    ESP_LOGI(TAG, "=== peripherals init done ===");
    return ESP_OK;
}
