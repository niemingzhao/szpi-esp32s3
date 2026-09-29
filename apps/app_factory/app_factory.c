/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Factory（APP-FACTORY 工厂模式）
 *
 * 自检 + 危险操作。自检项：屏幕（纯色循环）、触摸（跟手坐标）、音频（提示音）、
 * IMU / Wi-Fi / 蓝牙 / 存储 / 摄像头（状态行，随 1 秒定时器刷新）。
 * 危险操作：清除崩溃记录、恢复出厂设置（擦 NVS + 格式化内置 SPIFFS + 重启，两道确认）。
 *
 * PRD 提到"BOOT 长按启动"，那一层是启动路径（fw_input / main），这里是普通 App 入口。
 */

#include "app_factory.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.factory";

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_imu_row = NULL;
static lv_obj_t *s_wifi_row = NULL;
static lv_obj_t *s_bt_row = NULL;
static lv_obj_t *s_store_row = NULL;
static lv_obj_t *s_cam_row = NULL;
static lv_obj_t *s_overlay = NULL;
static lv_timer_t *s_timer = NULL;

/* ------------------------------ 屏幕测试 ------------------------------ */

static const uint32_t k_screen_colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000 };
static uint8_t s_screen_step = 0;

static void screen_test_close(void)
{
    if (s_overlay == NULL) return;

    lvgl_port_lock(0);
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    lvgl_port_unlock();
}

static void screen_test_cb(lv_event_t *e)
{
    (void)e;

    s_screen_step++;
    if (s_screen_step >= sizeof(k_screen_colors) / sizeof(k_screen_colors[0])) {
        s_screen_step = 0;
        screen_test_close();
        fw_ui_toast("屏幕测试结束", 1500);
        return;
    }

    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(k_screen_colors[s_screen_step]), 0);
    lvgl_port_unlock();
}

static void screen_test_start(lv_event_t *e)
{
    (void)e;

    if (s_overlay != NULL) return;

    s_screen_step = 0;

    lvgl_port_lock(0);
    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(k_screen_colors[0]), 0);
    lv_obj_add_event_cb(s_overlay, screen_test_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lvgl_port_unlock();

    fw_ui_toast("点屏幕切换颜色，5 次后结束", 2500);
}

/* ------------------------------ 触摸测试 ------------------------------ */

static void touch_close_cb(lv_event_t *e)
{
    (void)e;

    if (s_overlay == NULL) return;
    lvgl_port_lock(0);
    lv_obj_del(s_overlay);
    s_overlay = NULL;
    lvgl_port_unlock();
    fw_ui_toast("触摸测试结束", 1500);
}

static void touch_press_cb(lv_event_t *e)
{
    lv_obj_t *area = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t *dot = lv_obj_get_child(area, 0);
    lv_obj_t *info = lv_obj_get_child(area, 1);
    if (dot != NULL) lv_obj_set_pos(dot, p.x - 8, p.y - 8);
    if (info != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "x=%d y=%d", (int)p.x, (int)p.y);
        lv_label_set_text(info, buf);
    }
}

static void touch_test_start(lv_event_t *e)
{
    (void)e;

    if (s_overlay != NULL) return;

    lvgl_port_lock(0);

    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_overlay, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_add_event_cb(s_overlay, touch_press_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *dot = lv_obj_create(s_overlay);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, fw_theme_color_accent(), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_pos(dot, 0, 0);

    lv_obj_t *info = lv_label_create(s_overlay);
    lv_label_set_text(info, "在屏幕上滑动");
    lv_obj_set_style_text_font(info, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(info, fw_theme_color_text_secondary(), 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *close = lv_btn_create(s_overlay);
    lv_obj_set_size(close, 80, 34);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(close, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_border_color(close, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(close, touch_close_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *close_l = lv_label_create(close);
    lv_label_set_text(close_l, "结束");
    lv_obj_set_style_text_font(close_l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(close_l, fw_theme_color_text_primary(), 0);
    lv_obj_center(close_l);

    lvgl_port_unlock();
}

/* ------------------------------ 单项测试 ------------------------------ */

static void audio_test_cb(lv_event_t *e)
{
    (void)e;

    svc_audio_set_mute(false);
    if (svc_audio_play_tone_async(1000, 400) == ESP_OK) {
        fw_ui_toast("已播放 1 kHz 提示音", 1500);
    } else {
        fw_ui_toast("播放失败", 1500);
    }
}

static void camera_test_cb(lv_event_t *e)
{
    (void)e;
    if (s_cam_row == NULL) return;

    lvgl_port_lock(0);
    fw_ui_row_btn_value(s_cam_row, "测试中…");
    lvgl_port_unlock();

    esp_err_t err = svc_camera_init();
    if (err == ESP_OK) {
        svc_camera_frame_t f;
        err = svc_camera_capture(&f);
        if (err == ESP_OK) svc_camera_release();
        svc_camera_deinit();
    }

    lvgl_port_lock(0);
    fw_ui_row_btn_value(s_cam_row, (err == ESP_OK) ? "正常" : "失败");
    lvgl_port_unlock();

    fw_ui_toast((err == ESP_OK) ? "摄像头正常" : "摄像头失败", 2000);
}

/* ------------------------------ 危险操作 ------------------------------ */

static void factory_reset_final(fw_dialog_btn_t btn, void *user)
{
    (void)user;

    if (btn != FW_DIALOG_BTN_OK) return;

    ESP_LOGW(TAG, "factory reset: erase NVS + format internal SPIFFS");

    svc_settings_factory_reset();
    svc_storage_format(PERIPH_STORAGE_INTERNAL_FLASH);

    fw_ui_toast("正在重启…", 2000);
    svc_power_request_reboot();
}

static void factory_reset_warn(fw_dialog_btn_t btn, void *user)
{
    (void)user;

    if (btn != FW_DIALOG_BTN_OK) return;

    fw_ui_dialog(NULL, "再次确认", "所有设置、Wi-Fi 凭据和内置存储都会清空，且无法恢复。",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, factory_reset_final, NULL);
}

static void factory_reset_cb(lv_event_t *e)
{
    (void)e;

    fw_ui_dialog(NULL, "恢复出厂设置", "擦除 NVS 配置 + 格式化内置存储，然后自动重启。",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, factory_reset_warn, NULL);
}

static void clear_crash_cb(lv_event_t *e)
{
    (void)e;

    if (svc_sysinfo_clear_crash_log() == ESP_OK) {
        fw_ui_toast("崩溃记录已清除", 1500);
    } else {
        fw_ui_toast("清除失败", 1500);
    }
}

/* ------------------------------ 状态刷新 ------------------------------ */

static void refresh_status(void)
{
    /* IMU */
    if (s_imu_row != NULL) {
        periph_imu_data_t d;
        char buf[64];
        if (svc_imu_read(&d) == ESP_OK) {
            snprintf(buf, sizeof(buf), "X %+.0f  Y %+.0f  Z %+.0f%s",
                     d.roll, d.pitch, d.yaw, svc_imu_is_moving() ? "  运动" : "");
        } else {
            snprintf(buf, sizeof(buf), "读取失败");
        }
        fw_ui_row_btn_value(s_imu_row, buf);
    }

    /* Wi-Fi */
    if (s_wifi_row != NULL) {
        svc_net_status_t st;
        char buf[80];
        if (svc_net_get_status(&st) == ESP_OK && st.wifi_connected) {
            snprintf(buf, sizeof(buf), "%s  %d dBm", st.wifi_ssid, (int)st.rssi);
        } else {
            snprintf(buf, sizeof(buf), "未连接");
        }
        fw_ui_row_btn_value(s_wifi_row, buf);
    }

    /* 蓝牙 */
    if (s_bt_row != NULL) {
        svc_bt_diag_t d;
        char buf[80];
        if (svc_bt_get_diag(&d) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%s%s", svc_bt_state_name(d.state),
                     d.connected ? "（已连接）" : "");
        } else {
            snprintf(buf, sizeof(buf), "不可用");
        }
        fw_ui_row_btn_value(s_bt_row, buf);
    }

    /* 存储 */
    if (s_store_row != NULL) {
        periph_storage_info_t tf, in;
        char buf[96];
        bool tf_ok = (svc_storage_get_info(PERIPH_STORAGE_TF_CARD, &tf) == ESP_OK);
        bool in_ok = (svc_storage_get_info(PERIPH_STORAGE_INTERNAL_FLASH, &in) == ESP_OK);

        if (tf_ok && in_ok) {
            snprintf(buf, sizeof(buf), "TF %u/%u MB  内置 %u/%u MB",
                     (unsigned)((tf.total_bytes - tf.free_bytes) / (1024 * 1024)),
                     (unsigned)(tf.total_bytes / (1024 * 1024)),
                     (unsigned)((in.total_bytes - in.free_bytes) / (1024)),
                     (unsigned)(in.total_bytes / (1024)));
        } else if (tf_ok) {
            snprintf(buf, sizeof(buf), "TF 剩余 %u MB", (unsigned)(tf.free_bytes / (1024 * 1024)));
        } else {
            snprintf(buf, sizeof(buf), "TF 卡不可用");
        }
        fw_ui_row_btn_value(s_store_row, buf);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_status();
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *factory_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    fw_ui_row_btn(body, LV_SYMBOL_IMAGE, "屏幕测试", screen_test_start, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_EDIT, "触摸测试", touch_test_start, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_AUDIO, "音频测试", audio_test_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_IMAGE, "摄像头测试", camera_test_cb, NULL);

    s_imu_row = fw_ui_row_btn(body, LV_SYMBOL_GPS, "IMU", NULL, NULL);
    s_wifi_row = fw_ui_row_btn(body, LV_SYMBOL_WIFI, "Wi-Fi", NULL, NULL);
    s_bt_row = fw_ui_row_btn(body, LV_SYMBOL_BLUETOOTH, "蓝牙", NULL, NULL);
    s_store_row = fw_ui_row_btn(body, LV_SYMBOL_DRIVE, "存储", NULL, NULL);
    s_cam_row = fw_ui_row_btn(body, LV_SYMBOL_FILE, "摄像头状态", NULL, NULL);

    fw_ui_row_btn(body, LV_SYMBOL_TRASH, "清除崩溃记录", clear_crash_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_WARNING, "恢复出厂设置", factory_reset_cb, NULL);

    refresh_status();

    s_timer = lv_timer_create(timer_cb, 1000, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void factory_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void factory_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh_status();
}

static void factory_on_destroy(void *ctx)
{
    (void)ctx;

    if (s_overlay != NULL) {
        lvgl_port_lock(0);
        lv_obj_del(s_overlay);
        s_overlay = NULL;
        lvgl_port_unlock();
    }

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_imu_row = NULL;
    s_wifi_row = NULL;
    s_bt_row = NULL;
    s_store_row = NULL;
    s_cam_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_factory_desc = {
    .name = "Factory",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_SETTINGS,
    .on_create = factory_on_create,
    .on_pause = factory_on_pause,
    .on_resume = factory_on_resume,
    .on_destroy = factory_on_destroy,
};
