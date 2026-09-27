/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * App 图标声明（实现由 tools/gen_fw_icons.py 生成到 assets/fw_icons.c）
 *
 * icon_ui_*      20x20，白色 + alpha：界面功能图标，运行时按主题 / 用途染色
 *                （fw_ui_icon() / fw_ui_icon_btn() / fw_ui_row_btn_img()）
 * icon_home_*    40x40，彩色：桌面 App 图标（fw_app_desc_t.icon）
 * icon_status_*  20x20，白色 + alpha：状态栏状态图标
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_IMG_DECLARE(icon_ui_left);
LV_IMG_DECLARE(icon_ui_right);
LV_IMG_DECLARE(icon_ui_left2);
LV_IMG_DECLARE(icon_ui_right2);
LV_IMG_DECLARE(icon_ui_back);
LV_IMG_DECLARE(icon_ui_home);
LV_IMG_DECLARE(icon_ui_refresh);
LV_IMG_DECLARE(icon_ui_loop);
LV_IMG_DECLARE(icon_ui_search);
LV_IMG_DECLARE(icon_ui_gear);
LV_IMG_DECLARE(icon_ui_wifi);
LV_IMG_DECLARE(icon_ui_bt);
LV_IMG_DECLARE(icon_ui_play);
LV_IMG_DECLARE(icon_ui_pause);
LV_IMG_DECLARE(icon_ui_keyboard);
LV_IMG_DECLARE(icon_ui_trash);
LV_IMG_DECLARE(icon_ui_upload);
LV_IMG_DECLARE(icon_ui_download);
LV_IMG_DECLARE(icon_ui_pin);
LV_IMG_DECLARE(icon_ui_clock);
LV_IMG_DECLARE(icon_ui_eye);
LV_IMG_DECLARE(icon_ui_sun);
LV_IMG_DECLARE(icon_ui_contrast);
LV_IMG_DECLARE(icon_ui_moon);
LV_IMG_DECLARE(icon_ui_cloud);
LV_IMG_DECLARE(icon_ui_rain);
LV_IMG_DECLARE(icon_ui_snow);
LV_IMG_DECLARE(icon_ui_fog);
LV_IMG_DECLARE(icon_ui_thunder);
LV_IMG_DECLARE(icon_ui_up);
LV_IMG_DECLARE(icon_ui_more);
LV_IMG_DECLARE(icon_ui_file);
LV_IMG_DECLARE(icon_ui_file_text);
LV_IMG_DECLARE(icon_ui_file_audio);
LV_IMG_DECLARE(icon_ui_file_image);
LV_IMG_DECLARE(icon_ui_camera);
LV_IMG_DECLARE(icon_ui_zoom_in);
LV_IMG_DECLARE(icon_ui_zoom_out);
LV_IMG_DECLARE(icon_ui_save);
LV_IMG_DECLARE(icon_ui_folder);
LV_IMG_DECLARE(icon_ui_folder_plus);
LV_IMG_DECLARE(icon_ui_link);
LV_IMG_DECLARE(icon_ui_lock);
LV_IMG_DECLARE(icon_ui_mute);
LV_IMG_DECLARE(icon_ui_speaker);
LV_IMG_DECLARE(icon_ui_mic);
LV_IMG_DECLARE(icon_ui_stop);
LV_IMG_DECLARE(icon_ui_flag);
LV_IMG_DECLARE(icon_ui_warning);
LV_IMG_DECLARE(icon_ui_filter);
LV_IMG_DECLARE(icon_ui_hotspot);
LV_IMG_DECLARE(icon_ui_phone);
LV_IMG_DECLARE(icon_ui_check);
LV_IMG_DECLARE(icon_ui_close);

LV_IMG_DECLARE(icon_home_scripts);
LV_IMG_DECLARE(icon_home_clock);
LV_IMG_DECLARE(icon_home_calendar);
LV_IMG_DECLARE(icon_home_weather);
LV_IMG_DECLARE(icon_home_wifi);
LV_IMG_DECLARE(icon_home_bt);
LV_IMG_DECLARE(icon_home_display);
LV_IMG_DECLARE(icon_home_sound);
LV_IMG_DECLARE(icon_home_file);
LV_IMG_DECLARE(icon_home_editor);
LV_IMG_DECLARE(icon_home_calc);
LV_IMG_DECLARE(icon_home_download);
LV_IMG_DECLARE(icon_home_music);
LV_IMG_DECLARE(icon_home_recorder);
LV_IMG_DECLARE(icon_home_camera);
LV_IMG_DECLARE(icon_home_image);
LV_IMG_DECLARE(icon_home_stopwatch);
LV_IMG_DECLARE(icon_home_timer);
LV_IMG_DECLARE(icon_home_imu);
LV_IMG_DECLARE(icon_home_perf);
LV_IMG_DECLARE(icon_home_log);
LV_IMG_DECLARE(icon_home_about);

LV_IMG_DECLARE(icon_status_wifi);
LV_IMG_DECLARE(icon_status_bt);
LV_IMG_DECLARE(icon_status_sound);
LV_IMG_DECLARE(icon_status_record);
LV_IMG_DECLARE(icon_status_camera);
LV_IMG_DECLARE(icon_status_sd);

#ifdef __cplusplus
}
#endif
