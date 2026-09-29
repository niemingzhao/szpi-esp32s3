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

    ESP_ERROR_CHECK(svc_event_bus_init());
    ESP_LOGI(TAG, "event_bus initialized");

    ESP_ERROR_CHECK(svc_settings_init());
    ESP_ERROR_CHECK(svc_storage_init());

    ESP_ERROR_CHECK(svc_time_init());
    ESP_LOGI(TAG, "time initialized");

    ESP_ERROR_CHECK(svc_audio_init());
    ESP_ERROR_CHECK(svc_net_init());

    ESP_ERROR_CHECK(svc_power_init());
    ESP_LOGI(TAG, "power initialized");

    ESP_ERROR_CHECK(svc_notification_init());
    ESP_LOGI(TAG, "notification initialized");

    ESP_LOGI(TAG, "=== services init done ===");
    return ESP_OK;
}
