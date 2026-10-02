/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - IMU（APP-IMU 姿态仪）
 *
 * 每 150 ms 读一次 QMI8658（走 svc_imu）：上面是水平仪（圆 + 气泡随倾角移动），
 * 下面是三轴角度、加速度原始值与运动 / 朝向状态。只显示，不记录。
 */

#include "app_imu.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>

static const char *TAG = "app.imu";

#define IMU_LEVEL_MAX   60      /* 气泡最大偏移（px） */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_bubble = NULL;
static lv_obj_t *s_angles = NULL;
static lv_obj_t *s_accel = NULL;
static lv_obj_t *s_state = NULL;
static lv_timer_t *s_timer = NULL;

static int clamp_offset(float deg)
{
    int v = (int)(deg * 1.3f);
    if (v > IMU_LEVEL_MAX) v = IMU_LEVEL_MAX;
    if (v < -IMU_LEVEL_MAX) v = -IMU_LEVEL_MAX;
    return v;
}

static void refresh(void)
{
    periph_imu_data_t d;
    if (svc_imu_read(&d) != ESP_OK) {
        if (s_state != NULL) lv_label_set_text(s_state, "IMU 读取失败");
        return;
    }

    if (s_bubble != NULL) {
        lv_obj_align(s_bubble, LV_ALIGN_CENTER, -clamp_offset(d.roll), clamp_offset(d.pitch));
    }

    if (s_angles != NULL) {
        char buf[80];
        snprintf(buf, sizeof(buf), "X %+6.1f   Y %+6.1f   Z %+6.1f",
                 d.roll, d.pitch, d.yaw);
        lv_label_set_text(s_angles, buf);
    }

    if (s_accel != NULL) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f  m/s2", d.acc_x, d.acc_y, d.acc_z);
        lv_label_set_text(s_accel, buf);
    }

    if (s_state != NULL) {
        const char *moving = svc_imu_is_moving() ? "运动" : "静止";
        const char *orient = "竖屏";
        switch (svc_imu_get_orientation()) {
        case SVC_IMU_ORIENTATION_LANDSCAPE:      orient = "横屏"; break;
        case SVC_IMU_ORIENTATION_PORTRAIT_FLIP:  orient = "竖屏翻转"; break;
        case SVC_IMU_ORIENTATION_LANDSCAPE_FLIP: orient = "横屏翻转"; break;
        default:                                    orient = "竖屏"; break;
        }

        char buf[48];
        snprintf(buf, sizeof(buf), "%s · %s", moving, orient);
        lv_label_set_text(s_state, buf);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void *imu_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 水平仪：外圈 + 气泡 */
    lv_obj_t *level = lv_obj_create(body);
    lv_obj_set_size(level, 140, 140);
    lv_obj_set_scrollable(level, false);
    lv_obj_set_style_radius(level, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(level, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(level, 2, 0);
    lv_obj_set_style_border_color(level, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(level, 0, 0);

    s_bubble = lv_obj_create(level);
    lv_obj_set_size(s_bubble, 26, 26);
    lv_obj_set_scrollable(s_bubble, false);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bubble, fw_theme_color_accent(), 0);
    lv_obj_set_style_border_width(s_bubble, 0, 0);
    lv_obj_align(s_bubble, LV_ALIGN_CENTER, 0, 0);

    s_angles = lv_label_create(body);
    lv_obj_set_style_text_font(s_angles, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(s_angles, fw_theme_color_text_primary(), 0);

    s_accel = lv_label_create(body);
    lv_obj_set_style_text_font(s_accel, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_accel, fw_theme_color_text_secondary(), 0);

    s_state = lv_label_create(body);
    lv_obj_set_style_text_font(s_state, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state, fw_theme_color_success(), 0);

    refresh();
    s_timer = lv_timer_create(timer_cb, 150, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void imu_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void imu_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh();
}

static void imu_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_bubble = NULL;
    s_angles = NULL;
    s_accel = NULL;
    s_state = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_imu_desc = {
    .name = "IMU",
    .title = "姿态仪",
    .icon_64 = &icon_home_imu,
    .symbol = LV_SYMBOL_GPS,
    .on_create = imu_on_create,
    .on_pause = imu_on_pause,
    .on_resume = imu_on_resume,
    .on_destroy = imu_on_destroy,
};
