/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Display（APP-DISPLAY 显示）
 *
 * 亮度滑块 + 熄屏超时 + 主题切换。亮度 / 熄屏超时由 svc_power 持久化（NVS namespace sys）；
 * 主题由 fw_theme 持久化并广播 SVC_EVENT_THEME_CHANGED，界面重建不在本 App 内做。
 *
 * 三个设置项用 fw_ui_slider_row / fw_ui_row_btn 的通用行（高 30 + 2 × 50），一屏放下不滚动。
 */

#include "app_display.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "app.display";

/* 熄屏超时预设（秒），0 = 不熄屏 */
static const uint32_t TIMEOUTS[] = { 15, 30, 60, 120, 300, 0 };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_timeout_row = NULL;
static lv_obj_t *s_theme_row = NULL;

static void timeout_label(char *buf, size_t len, uint32_t seconds)
{
    if (seconds == 0) {
        snprintf(buf, len, "不熄屏");
    } else if (seconds < 60) {
        snprintf(buf, len, "%u 秒", (unsigned)seconds);
    } else {
        snprintf(buf, len, "%u 分钟", (unsigned)(seconds / 60));
    }
}

static void theme_label(char *buf, size_t len)
{
    snprintf(buf, len, "%s", (fw_theme_current() == FW_THEME_LIGHT) ? "浅色" : "深色");
}

/* 滑块右侧显示当前百分比 */
static void brightness_show(lv_obj_t *slider)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)lv_slider_get_value(slider));
    fw_ui_slider_row_value(slider, buf);
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    svc_power_set_brightness((uint8_t)lv_slider_get_value(slider));
    brightness_show(slider);
}

static void timeout_cb(lv_event_t *e)
{
    (void)e;

    const uint32_t cur = svc_power_get_backlight_timeout();
    const size_t n = sizeof(TIMEOUTS) / sizeof(TIMEOUTS[0]);

    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        if (TIMEOUTS[i] == cur) {
            idx = i;
            break;
        }
    }

    const uint32_t next = TIMEOUTS[(idx + 1) % n];
    svc_power_set_backlight_timeout(next);

    char buf[24];
    timeout_label(buf, sizeof(buf), next);
    fw_ui_row_btn_value(s_timeout_row, buf);
}

/* 切换主题：只调用 fw_theme_apply()（改调色板 + 写 NVS + 发 SVC_EVENT_THEME_CHANGED）。
 * 界面重建由 fw_init 订阅该事件后用 lv_async_call 完成（AGENTS 4.12）——这里不能自己删
 * 控件，否则会删掉正在处理本事件的这个按钮；回调返回后本 App 会 on_destroy → on_create
 * 重建，右侧数值由 display_on_create() 按新主题重填，所以这里不碰旧控件指针。 */
static void theme_cb(lv_event_t *e)
{
    (void)e;

    fw_theme_apply((fw_theme_current() == FW_THEME_LIGHT) ? FW_THEME_DARK : FW_THEME_LIGHT);
}

static void *display_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *group = fw_ui_group(body);

    lv_obj_t *slider =
        fw_ui_slider_row(group, &icon_ui_sun, "亮度", 0, 100, svc_power_get_brightness(),
                         brightness_cb, NULL);
    brightness_show(slider);

    s_timeout_row = fw_ui_row_btn_img(group, &icon_ui_moon, "熄屏超时", timeout_cb, NULL);
    char buf[24];
    timeout_label(buf, sizeof(buf), svc_power_get_backlight_timeout());
    fw_ui_row_btn_value(s_timeout_row, buf);

    s_theme_row = fw_ui_row_btn_img(group, &icon_ui_contrast, "主题", theme_cb, NULL);
    char tbuf[12];
    theme_label(tbuf, sizeof(tbuf));
    fw_ui_row_btn_value(s_theme_row, tbuf);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void display_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_timeout_row = NULL;
    s_theme_row = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_display_desc = {
    .name = "Display",
    .title = "显示",
    .icon_64 = &icon_home_display,
    .symbol = LV_SYMBOL_EYE_OPEN,
    .on_create = display_on_create,
    .on_destroy = display_on_destroy,
};
