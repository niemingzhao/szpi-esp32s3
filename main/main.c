/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS v0.4 - Framework Layer Integration
 *
 * 启动序列：
 *   NVS → bsp_init()（Drivers）→ peripherals_init_all()（含 LVGL display/touch）
 *   → services_init() → fw_init() → app_register_all()
 *   → fw_app_mgr_launch("Home") → 挂载内置 Flash（首次自动格式化）
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "drv_common.h"
#include "periph_common.h"
#include "svc_common.h"
#include "fw_common.h"
#include "app_common.h"

static const char *TAG = "szpi-os";

void app_main(void)
{
    ESP_LOGI(TAG, "===== SZPI-OS Boot =====");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "SZPI-OS %s / %s", SZPI_OS_VERSION, SZPI_OS_LAYER);

    // 1. NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Drivers 层初始化（I2C / SPI / LEDC / PCA9557 / LCD / Touch / Key / IMU）
    ESP_ERROR_CHECK(bsp_init(NULL, NULL, NULL));

    // 3. Peripherals 层初始化（含 LVGL display 与触摸 input device）
    ESP_ERROR_CHECK(peripherals_init_all());

    // 4. Services 层初始化
    ESP_ERROR_CHECK(services_init());

    // 5. Framework 层初始化（主题 / 资源 / 窗口 / App 管理 / 状态栏 / 输入）
    ESP_ERROR_CHECK(fw_init());

    // 6. 注册所有内置 App 并启动桌面
    app_register_all();
    ESP_ERROR_CHECK(fw_app_mgr_launch(FW_APP_HOME_NAME));
    ESP_LOGI(TAG, "Home launched");

    // 7. 挂载内置 Flash 文件系统（首次自动格式化；放在首屏之后避免阻塞）
    if (periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH) != ESP_OK) {
        ESP_LOGW(TAG, "internal storage mount failed");
    }

    ESP_LOGI(TAG, "===== Ready =====");
}
