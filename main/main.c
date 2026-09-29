/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS v0.5 - Services Layer (net + audio)
 *
 * 启动序列：
 *   NVS → bsp_init()（Drivers）→ peripherals_init_all()（含 LVGL display/touch）
 *   → services_init() → fw_init() → app_register_all() → fw_boot_animation()
 *   → fw_app_mgr_launch("Home") → 挂载内置 Flash（首次自动格式化）→ 打开看门狗
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
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
    ESP_LOGI(TAG, "SZPI-OS %s", SZPI_OS_VERSION);

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

    // 6. 注册所有内置 App，播放启动动画，然后启动桌面
    app_register_all();
    ESP_ERROR_CHECK(fw_boot_animation());
    ESP_ERROR_CHECK(fw_app_mgr_launch(FW_APP_HOME_NAME));
    ESP_LOGI(TAG, "Home launched");

    // 7. 挂载内置 Flash 文件系统（首次自动格式化；放在首屏之后避免阻塞）
    //    这里也是内部内存最紧的时刻（Wi-Fi 刚连上、BLE 在广播），打一条余量便于排查。
    //    容量一律用 MALLOC_CAP_DMA：它正好是"内部 DMA 可用区"，也是 BLE/音频/帧缓冲/FreeRTOS
    //    对象真正会耗的那块；MALLOC_CAP_INTERNAL 会把 IRAM 也算进来，largest 会大得没有意义。
    ESP_LOGI(TAG, "heap before internal mount: dma-internal free=%u largest=%u, psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH) != ESP_OK) {
        ESP_LOGW(TAG, "internal storage mount failed");
    }

    // 8. 启动完成，打开看门狗的超时自动重启（SYS-004）
    if (svc_watchdog_arm() != ESP_OK) {
        ESP_LOGW(TAG, "watchdog arm failed");
    }

    ESP_LOGI(TAG, "===== Ready =====");
}
