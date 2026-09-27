/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS 启动入口
 *
 * 启动序列：
 *   NVS → bsp_init()（Drivers）→ peripherals_init_all()（含 LVGL display/touch）
 *   → services_init() → fw_init() → app_register_all() → fw_boot_animation()
 *   → fw_app_mgr_launch("Home") → 挂载内置 Flash（首次自动格式化）→ 打开看门狗
 */

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
    /* 第一件事就装日志钩子：这样「系统日志」里能看到完整的开机过程（环里只留最近 2 KB） */
    svc_sysinfo_log_capture_start();

    ESP_LOGI(TAG, "===== " SZPI_OS_NAME " Boot =====");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "%s %s", SZPI_OS_NAME, SZPI_OS_VERSION);

    // 1. NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Drivers 层初始化（I2C / SPI / LEDC / PCA9557 / LCD / Touch / Key / IMU）
    ESP_ERROR_CHECK(bsp_init());

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

    // 7. 挂载内置 Flash 文件系统（首次挂载会自动格式化；放在首屏之后避免阻塞）
    //    此刻内部内存最紧，打印一次 DMA 可用量便于排查（MALLOC_CAP_DMA 即内部 DMA 区）
    ESP_LOGI(TAG, "heap before internal mount: dma-internal free=%u largest=%u, psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH) != ESP_OK) {
        ESP_LOGW(TAG, "internal storage mount failed");
    }

    // 8. 启动完成，打开看门狗的超时自动重启
    if (svc_watchdog_arm() != ESP_OK) {
        ESP_LOGW(TAG, "watchdog arm failed");
    }

    ESP_LOGI(TAG, "===== Ready =====");
}
