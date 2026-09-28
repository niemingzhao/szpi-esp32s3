/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - App 注册入口
 *
 * Phase 1 手动注册：集中登记所有内置 App。
 * Phase 2 可改为 FW_APP_REGISTER 段注册。
 */

#include "app_common.h"
#include "app_home.h"
#include "app_clock.h"
#include "app_settings.h"
#include "fw_common.h"
#include "esp_log.h"

static const char *TAG = "app";

void app_register_all(void)
{
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_home_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_clock_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_settings_desc));

    ESP_LOGI(TAG, "registered %u apps", (unsigned)fw_app_mgr_count());
}
