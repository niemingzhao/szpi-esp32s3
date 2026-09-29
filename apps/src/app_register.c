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
#include "app_ble.h"
#include "app_music.h"
#include "app_recorder.h"
#include "app_image.h"
#include "app_video.h"
#include "app_camera.h"
#include "app_file.h"
#include "app_editor.h"
#include "app_calc.h"
#include "app_imu.h"
#include "app_browser.h"
#include "app_ota.h"
#include "app_debug.h"
#include "app_about.h"
#include "app_factory.h"
#include "fw_common.h"
#include "esp_log.h"

static const char *TAG = "app.registry";

void app_register_all(void)
{
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_home_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_clock_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_settings_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_ble_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_music_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_recorder_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_image_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_video_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_camera_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_file_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_editor_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_calc_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_imu_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_browser_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_ota_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_debug_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_about_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_factory_desc));

    ESP_LOGI(TAG, "registered %u apps", (unsigned)fw_app_mgr_count());
}
