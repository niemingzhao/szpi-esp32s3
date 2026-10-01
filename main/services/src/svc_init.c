/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 初始化入口
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "svc.init";

esp_err_t services_init(void)
{
    ESP_LOGI(TAG, "=== services init start ===");

    /* 看门狗：失败不阻塞启动；超时自动重启在启动末尾由 main 打开 */
    if (svc_watchdog_init() != ESP_OK) {
        ESP_LOGW(TAG, "watchdog unavailable");
    }

    ESP_ERROR_CHECK(svc_event_bus_init());
    ESP_LOGI(TAG, "event_bus initialized");

    ESP_ERROR_CHECK(svc_settings_init());
    ESP_ERROR_CHECK(svc_storage_init());

    /* 蓝牙放最前面：控制器与主机（Bluedroid）都要内部 RAM，而且主机的工作队列 /
     * 任务栈不能放 PSRAM；晚于 Wi-Fi / LVGL 初始化会要不到连续内存，报
     * BLE_INIT: Malloc failed 或 btu_workqueue 失败。失败不影响启动。 */
    if (svc_bt_init() != ESP_OK) {
        ESP_LOGW(TAG, "bluetooth unavailable");
    }

    ESP_ERROR_CHECK(svc_time_init());
    ESP_LOGI(TAG, "time initialized");

    ESP_ERROR_CHECK(svc_audio_init());
    ESP_ERROR_CHECK(svc_net_init());

    ESP_ERROR_CHECK(svc_power_init());
    ESP_LOGI(TAG, "power initialized");

    ESP_ERROR_CHECK(svc_imu_init());
    ESP_LOGI(TAG, "imu initialized");

    ESP_ERROR_CHECK(svc_io_init());
    ESP_LOGI(TAG, "io initialized");

    /* 摄像头只做服务初始化，硬件按需打开（svc_camera_open） */
    ESP_ERROR_CHECK(svc_camera_init());

    ESP_ERROR_CHECK(svc_sysinfo_init());
    ESP_LOGI(TAG, "sysinfo initialized");

    ESP_LOGI(TAG, "=== services init done ===");
    return ESP_OK;
}
