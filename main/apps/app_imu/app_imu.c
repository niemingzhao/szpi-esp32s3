/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - IMU（姿态仪）
 *
 * 每 150 ms 读一次 QMI8658（走 svc_imu）：顶部一行是运动 / 朝向状态与「归零」按钮，
 * 中间是水平仪（圆 + 气泡随倾角移动），下面一张详情格是
 * 三个倾角（滚 / 俯 / 偏，°）、三轴加速度（加X/Y/Z，m/s²）与三轴角速度（角X/Y/Z，°/s）。
 *
 * 「归零」把当前姿态记成基准（水平仪居中、角度从 0 起算），再点一下恢复原始读数 ——
 * 等于给这块板子当水平仪用时先校平。只显示，不记录。
 */

#include "app_imu.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "app.imu";

#define IMU_REFRESH_MS  150
#define IMU_LEVEL_SIZE  72      /* 水平仪外圈直径 */
#define IMU_BUBBLE_SIZE 16
#define IMU_LEVEL_MAX   ((IMU_LEVEL_SIZE - IMU_BUBBLE_SIZE) / 2)   /* 气泡最大偏移（px） */

/* 详情格：3 行 × 3 列 */
static const char *const k_cells[9] = {
    "滚", "俯", "偏",
    "加X", "加Y", "加Z",
    "角X", "角Y", "角Z",
};

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_bubble = NULL;
static lv_obj_t *s_state = NULL;
static lv_obj_t *s_zero_lb = NULL;
static lv_obj_t *s_table = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_foreground = false;
static bool s_zeroed = false;
static float s_zero_roll = 0.0f;
static float s_zero_pitch = 0.0f;
static float s_zero_yaw = 0.0f;

static int clamp_offset(float deg)
{
    int v = (int)(deg * 1.0f);
    if (v > IMU_LEVEL_MAX) v = IMU_LEVEL_MAX;
    if (v < -IMU_LEVEL_MAX) v = -IMU_LEVEL_MAX;
    return v;
}

static void refresh(void)
{
    svc_imu_data_t d;
    if (svc_imu_read(&d) != ESP_OK) {
        if (s_state != NULL) lv_label_set_text(s_state, "IMU 读取失败");
        return;
    }

    const float roll = d.roll - s_zero_roll;
    const float pitch = d.pitch - s_zero_pitch;
    const float yaw = d.yaw - s_zero_yaw;

    if (s_bubble != NULL) {
        lv_obj_align(s_bubble, LV_ALIGN_CENTER, -clamp_offset(roll), clamp_offset(pitch));
    }

    if (s_table != NULL) {
        char buf[48];                   /* 浮点指令 GCC 按最坏情况算宽度，缓冲别贴着用 */
        snprintf(buf, sizeof(buf), "%+.1f", roll);
        fw_ui_table_value(s_table, 0, buf);
        snprintf(buf, sizeof(buf), "%+.1f", pitch);
        fw_ui_table_value(s_table, 1, buf);
        snprintf(buf, sizeof(buf), "%+.1f", yaw);
        fw_ui_table_value(s_table, 2, buf);

        snprintf(buf, sizeof(buf), "%+.2f", d.acc_x);
        fw_ui_table_value(s_table, 3, buf);
        snprintf(buf, sizeof(buf), "%+.2f", d.acc_y);
        fw_ui_table_value(s_table, 4, buf);
        snprintf(buf, sizeof(buf), "%+.2f", d.acc_z);
        fw_ui_table_value(s_table, 5, buf);

        snprintf(buf, sizeof(buf), "%+.1f", d.gyr_x);
        fw_ui_table_value(s_table, 6, buf);
        snprintf(buf, sizeof(buf), "%+.1f", d.gyr_y);
        fw_ui_table_value(s_table, 7, buf);
        snprintf(buf, sizeof(buf), "%+.1f", d.gyr_z);
        fw_ui_table_value(s_table, 8, buf);
    }

    if (s_state != NULL) {
        const char *moving = svc_imu_is_moving() ? "运动" : "静止";
        const char *orient = "竖屏";
        switch (svc_imu_get_orientation()) {
        case SVC_IMU_ORIENTATION_LANDSCAPE:      orient = "横屏"; break;
        case SVC_IMU_ORIENTATION_PORTRAIT_FLIP:  orient = "竖屏翻转"; break;
        case SVC_IMU_ORIENTATION_LANDSCAPE_FLIP: orient = "横屏翻转"; break;
        default:                                 orient = "竖屏"; break;
        }

        char buf[48];
        if (s_zeroed) {
            snprintf(buf, sizeof(buf), "已归零 · %s%s", moving, orient);
        } else {
            snprintf(buf, sizeof(buf), "%s · %s", moving, orient);
        }
        lv_label_set_text(s_state, buf);
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

/* 归零 / 复位基准：点一下把当前姿态记成基准，再点一下恢复原始读数 */
static void zero_cb(lv_event_t *e)
{
    (void)e;

    if (s_zeroed) {
        s_zeroed = false;
        /* 基准也要清掉：只把标志置假的话，读数仍按上次记下的基准做了偏移，
         * 页面上的气泡会一直停在归零位置 */
        s_zero_roll = 0.0f;
        s_zero_pitch = 0.0f;
        s_zero_yaw = 0.0f;
    } else {
        svc_imu_data_t d;
        if (svc_imu_read(&d) != ESP_OK) {
            fw_ui_toast("IMU 读取失败", 2000);
            return;
        }
        s_zero_roll = d.roll;
        s_zero_pitch = d.pitch;
        s_zero_yaw = d.yaw;
        s_zeroed = true;
    }

    if (s_zero_lb != NULL) lv_label_set_text(s_zero_lb, s_zeroed ? "复位" : "归零");
    fw_ui_toast(s_zeroed ? "已把当前姿态记为水平" : "已恢复原始读数", 1800);
    refresh();
}

static void *imu_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_foreground = false;
    s_zeroed = false;

    /* 顶部一行：运动 / 朝向状态 + 归零 */
    lv_obj_t *head = lv_obj_create(body);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_state = lv_label_create(head);
    lv_obj_set_width(s_state, 222);         /* 撑满除"归零"按钮外的宽度（DOT 截断要定宽） */
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_state, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state, fw_theme_color_success(), 0);

    lv_obj_t *zero = fw_ui_btn(head, "归零", 64, false, zero_cb, NULL);
    s_zero_lb = lv_obj_get_child(zero, 0);

    /* 水平仪：外圈 + 气泡 */
    lv_obj_t *level = lv_obj_create(body);
    lv_obj_set_size(level, IMU_LEVEL_SIZE, IMU_LEVEL_SIZE);
    lv_obj_set_scrollable(level, false);
    lv_obj_set_style_radius(level, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(level, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(level, 2, 0);
    lv_obj_set_style_border_color(level, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(level, 0, 0);

    s_bubble = lv_obj_create(level);
    lv_obj_set_size(s_bubble, IMU_BUBBLE_SIZE, IMU_BUBBLE_SIZE);
    lv_obj_set_scrollable(s_bubble, false);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bubble, fw_theme_color_accent(), 0);
    lv_obj_set_style_border_width(s_bubble, 0, 0);
    lv_obj_align(s_bubble, LV_ALIGN_CENTER, 0, 0);

    /* 详情格：倾角 / 加速度 / 角速度 */
    s_table = fw_ui_table(body, 3, 3, k_cells);

    refresh();

    /* 定时器先建好但不启用：等进入前台（on_start / on_resume）再恢复 */
    s_timer = lv_timer_create(timer_cb, IMU_REFRESH_MS, NULL);
    if (s_timer != NULL) lv_timer_pause(s_timer);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* on_start 与 on_resume 都挂这个：换主题重建时后台 App 也会走 on_create，
 * 而暂停过的定时器在"首次进入"与"从返回栈回来"两条路径上都要恢复 */
static void imu_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh();
}

static void imu_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);
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
    s_state = NULL;
    s_zero_lb = NULL;
    s_table = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_imu_desc = {
    .name = "IMU",
    .title = "姿态仪",
    .icon = &icon_home_imu,
    .on_create = imu_on_create,
    .on_start = imu_on_resume,
    .on_pause = imu_on_pause,
    .on_resume = imu_on_resume,
    .on_destroy = imu_on_destroy,
};
