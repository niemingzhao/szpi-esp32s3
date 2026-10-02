/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - App 注册入口
 *
 * 集中登记所有内置 App。新增 App 时：建 app_<name>/ 目录、在 apps/CMakeLists.txt
 * 的 SRC_DIRS / INCLUDE_DIRS 里登记，再在这里注册。
 *
 * 注册顺序 = 桌面网格顺序，必须与原始需求「内置应用」列表一致
 * （脚本管理、时钟、日历、天气、Wi-Fi、蓝牙 BLE、显示、声音、文件管理、
 * 文本编辑、计算器、下载器、音乐、录音机、相机、图库、秒表、
 * 计时器、姿态仪、性能监控、系统日志、关于本机）。
 * Home 排在最前，但它自己不出现在网格里。
 */

#include "app_common.h"
#include "app_home.h"
#include "app_scripts.h"
#include "app_clock.h"
#include "app_calendar.h"
#include "app_weather.h"
#include "app_wifi.h"
#include "app_bt.h"
#include "app_display.h"
#include "app_sound.h"
#include "app_file.h"
#include "app_editor.h"
#include "app_calc.h"
#include "app_download.h"
#include "app_music.h"
#include "app_recorder.h"
#include "app_camera.h"
#include "app_image.h"
#include "app_stopwatch.h"
#include "app_timer.h"
#include "app_imu.h"
#include "app_perf.h"
#include "app_log.h"
#include "app_about.h"
#include "fw_common.h"
#include "esp_log.h"

static const char *TAG = "app.registry";

void app_register_all(void)
{
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_home_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_scripts_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_clock_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_calendar_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_weather_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_wifi_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_bt_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_display_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_sound_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_file_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_editor_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_calc_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_download_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_music_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_recorder_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_camera_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_image_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_stopwatch_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_timer_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_imu_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_perf_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_log_desc));
    ESP_ERROR_CHECK(fw_app_mgr_register(&app_about_desc));

    ESP_LOGI(TAG, "registered %u apps", (unsigned)fw_app_mgr_count());
}
