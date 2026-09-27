/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Timer（计时器）
 *
 * 倒计时：预设时长循环切换（30 秒 ~ 60 分），顶部主信息卡显示剩余时间与状态，
 * 下面一行换时长，底部一条是开始暂停 / 复位。到点唤醒屏幕、弹提示并响三声
 * （svc_audio_play_tone_async，间隔 500 ms；服务侧会等功放启动斜坡再出声）。
 *
 * 计时基于 esp_timer_get_time()（微秒，单调），不用系统时间。
 * 离开 App 就停表（和音乐 / 录音机一致）：倒计时不会在后台自己走完、也不会在你
 * 看别的页面时突然响；回到 App 接着按「开始」继续。换主题是前台原地重建，不打断。
 */

#include "app_timer.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "app.timer";

#define REFRESH_MS      200
#define BEEP_HZ         880
#define BEEP_MS         200
#define BEEP_TIMES      3
#define BEEP_GAP_MS     500

/* 预设时长（秒） */
static const uint32_t PRESETS[] = { 30, 60, 120, 180, 300, 600, 900, 1800, 3600 };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time_lb = NULL;
static lv_obj_t *s_state_lb = NULL;
static lv_obj_t *s_preset_lb = NULL;
static lv_obj_t *s_dur_row = NULL;
static lv_obj_t *s_run_img = NULL;
static lv_obj_t *s_run_lb = NULL;
static lv_timer_t *s_timer = NULL;
static lv_timer_t *s_beep_timer = NULL;

static bool s_running = false;
static bool s_done = false;
static bool s_foreground = false;
static int64_t s_start_us = 0;      /* 本次开始时刻 */
static int64_t s_elapsed_us = 0;    /* 已经计掉的时长 */
static uint32_t s_total_s = 300;
static uint8_t s_beep_left = 0;

static int64_t remain_us(void)
{
    int64_t used = s_elapsed_us;
    if (s_running) used += esp_timer_get_time() - s_start_us;

    const int64_t total = (int64_t)s_total_s * 1000000;
    return (used >= total) ? 0 : (total - used);
}

static void fmt_duration(uint32_t sec, char *buf, size_t len)
{
    if (sec < 60) snprintf(buf, len, "%u 秒", (unsigned)sec);
    else if (sec % 60 == 0) snprintf(buf, len, "%u 分", (unsigned)(sec / 60));
    else snprintf(buf, len, "%u 分 %u 秒", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

static void show_remain(void)
{
    char buf[40];

    if (s_time_lb != NULL) {
        const int sec = (int)((remain_us() + 999999) / 1000000);   /* 向上取整，最后 1 秒不早跳 0 */
        snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
        lv_label_set_text(s_time_lb, buf);
    }

    if (s_state_lb != NULL) {
        const char *st = s_done ? "时间到" : (s_running ? "倒计时中" : "已暂停");
        lv_label_set_text(s_state_lb, st);
        lv_obj_set_style_text_color(s_state_lb,
                                    s_done ? fw_theme_color_error()
                                           : (s_running ? fw_theme_color_success()
                                                        : fw_theme_color_text_secondary()), 0);
    }

    if (s_run_lb != NULL) lv_label_set_text(s_run_lb, s_running ? "暂停" : "开始");
    if (s_run_img != NULL) {
        lv_image_set_src(s_run_img, s_running ? &icon_ui_pause : &icon_ui_play);
    }

    if (s_preset_lb != NULL) {
        fmt_duration(s_total_s, buf, sizeof(buf));
        char line[64];
        snprintf(line, sizeof(line), "预设 %s", buf);
        lv_label_set_text(s_preset_lb, line);
    }
}

/* ------------------------------- 到点提示 ------------------------------- */

static void beep_cb(lv_timer_t *t)
{
    svc_audio_play_tone_async(BEEP_HZ, BEEP_MS);

    if (--s_beep_left == 0) {
        s_beep_timer = NULL;
        if (t != NULL) lv_timer_delete(t);
    }
}

/* 响三声：第一声立刻响，剩下两声交给定时器 */
static void beep_start(void)
{
    svc_audio_play_tone_async(BEEP_HZ, BEEP_MS);
    s_beep_left = BEEP_TIMES - 1;

    if (s_beep_left > 0 && s_beep_timer == NULL) {
        s_beep_timer = lv_timer_create(beep_cb, BEEP_GAP_MS, NULL);
    }
}

static void done_notify(void)
{
    s_done = true;
    s_running = false;
    s_elapsed_us = (int64_t)s_total_s * 1000000;

    /* 亮屏 + 提示：屏可能在倒计时期间自动熄掉了 */
    svc_power_wake();
    fw_ui_toast("时间到", 3000);
    beep_start();
    show_remain();
}

/* ------------------------------- 界面 ------------------------------- */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    if (s_running && remain_us() == 0) {
        done_notify();
        return;
    }
    if (s_running) show_remain();       /* 暂停 / 到点后不用动界面 */
}

static void duration_cb(lv_event_t *e)
{
    (void)e;

    const size_t n = sizeof(PRESETS) / sizeof(PRESETS[0]);
    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        if (PRESETS[i] == s_total_s) {
            idx = i;
            break;
        }
    }
    s_total_s = PRESETS[(idx + 1) % n];

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;

    char buf[40];
    fmt_duration(s_total_s, buf, sizeof(buf));
    if (s_dur_row != NULL) fw_ui_row_btn_value(s_dur_row, buf);

    show_remain();
}

static void run_cb(lv_event_t *e)
{
    (void)e;

    if (s_done) {                       /* 到点后再按就是重新开始 */
        s_elapsed_us = 0;
        s_done = false;
    }

    if (s_running) {
        s_elapsed_us += esp_timer_get_time() - s_start_us;
        s_running = false;
    } else {
        s_start_us = esp_timer_get_time();
        s_running = true;
    }
    show_remain();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;
    show_remain();
}

static void *timer_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;
    s_foreground = false;

    /* 主信息卡：剩余时间是这一页的主角 */
    lv_obj_t *card = fw_ui_hero_card(body, 64);

    lv_obj_t *line1 = lv_obj_create(card);
    lv_obj_set_size(line1, lv_pct(100), 26);
    lv_obj_set_scrollable(line1, false);
    lv_obj_set_style_bg_opa(line1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line1, 0, 0);
    lv_obj_set_style_pad_all(line1, 0, 0);
    lv_obj_set_flex_flow(line1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(line1, LV_ALIGN_TOP_LEFT, 0, 0);

    s_time_lb = lv_label_create(line1);
    lv_label_set_text(s_time_lb, "05:00");
    lv_obj_set_style_text_font(s_time_lb, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_time_lb, fw_theme_color_text_primary(), 0);

    s_state_lb = lv_label_create(line1);
    lv_obj_set_style_text_font(s_state_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_state_lb, fw_theme_color_text_secondary(), 0);

    s_preset_lb = lv_label_create(card);
    lv_obj_set_width(s_preset_lb, lv_pct(100));
    lv_label_set_long_mode(s_preset_lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_preset_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_preset_lb, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_preset_lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 设置行：点一下换下一个预设 */
    lv_obj_t *grp = fw_ui_group(body);
    s_dur_row = fw_ui_row_btn_img(grp, &icon_ui_clock, "时长", duration_cb, NULL);
    fw_ui_group_end(grp);

    /* 底部：开始暂停 / 复位 */
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), 44);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *run = fw_ui_action_btn(bar, &icon_ui_play, "开始", 0, run_cb, NULL);
    s_run_img = lv_obj_get_child(run, 0);
    s_run_lb = lv_obj_get_child(run, 1);
    fw_ui_action_btn(bar, &icon_ui_refresh, "复位", 0, reset_cb, NULL);

    char buf[24];
    fmt_duration(s_total_s, buf, sizeof(buf));
    fw_ui_row_btn_value(s_dur_row, buf);
    show_remain();

    /* 定时器先建好但不启用：等进入前台（on_start / on_resume）再恢复 */
    s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    if (s_timer != NULL) lv_timer_pause(s_timer);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (total=%us)", (unsigned)s_total_s);
    return s_root;
}

/* on_start 与 on_resume 都挂这个：换主题重建时后台 App 也会走 on_create，
 * 而暂停过的定时器在"首次进入"与"从返回栈回来"两条路径上都要恢复 */
static void timer_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    show_remain();
}

static void timer_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);

    /* 真的离开 App 就停表：倒计时不在后台自己走完、也不会在别的页面突然响。
     * 换主题是前台原地重建，is_foreground 仍为真，不打断 */
    if (!fw_app_mgr_is_foreground("Timer") && s_running) {
        s_elapsed_us += esp_timer_get_time() - s_start_us;
        s_running = false;
        show_remain();
    }
}

static void timer_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_beep_timer != NULL) {
        lv_timer_delete(s_beep_timer);
        s_beep_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_time_lb = NULL;
    s_state_lb = NULL;
    s_preset_lb = NULL;
    s_dur_row = NULL;
    s_run_img = NULL;
    s_run_lb = NULL;
    lvgl_port_unlock();

    s_running = false;
    s_done = false;
    s_elapsed_us = 0;
    s_beep_left = 0;
}

const fw_app_desc_t app_timer_desc = {
    .name = "Timer",
    .title = "计时器",
    .icon = &icon_home_timer,
    .on_create = timer_on_create,
    .on_start = timer_on_resume,
    .on_pause = timer_on_pause,
    .on_resume = timer_on_resume,
    .on_destroy = timer_on_destroy,
};
