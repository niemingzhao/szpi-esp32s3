/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Stopwatch（秒表）
 *
 * 计时基于 esp_timer_get_time()（微秒，单调）：改系统时间、熄屏都不受影响。
 * 顶部主信息卡是累计用时（24 px）与状态，中间是计次列表（右侧为该次分段用时，新计次
 * 自动滚到可见处），底部一条是开始暂停 / 计次 / 复位。
 *
 * 刷新定时器 50 ms 一次，只在 App 在前台时跑；离开 App 就停止计时（不再偷偷跑秒），
 * 回来按「开始」接着上次的累计继续 —— 和音乐 / 录音机一致：退出即停。
 *
 * 计次数据放堆上（LAP_MAX × 8 B），不在 App 里加大静态数组。
 */

#include "app_stopwatch.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "app.stopwatch";

#define REFRESH_MS      50
#define LAP_MAX         100

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time_lb = NULL;
static lv_obj_t *s_state_lb = NULL;
static lv_obj_t *s_lap_hint = NULL;
static lv_obj_t *s_run_img = NULL;
static lv_obj_t *s_run_lb = NULL;
static lv_obj_t *s_lap_list = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_running = false;
static bool s_foreground = false;
static int64_t s_start_us = 0;      /* 本次开始时刻 */
static int64_t s_accum_us = 0;      /* 之前累计 */

/* 每次按"计次"时记录的累计用时（微秒），堆上分配 */
static int64_t *s_laps = NULL;
static size_t s_lap_count = 0;

static int64_t elapsed_us(void)
{
    return s_accum_us + (s_running ? (esp_timer_get_time() - s_start_us) : 0);
}

/* 把微秒格式化成 分:秒.百分秒 */
static void format_time(int64_t us, char *buf, size_t len)
{
    const int64_t cs = us / 10000;        /* 百分秒 */
    snprintf(buf, len, "%02d:%02d.%02d",
             (int)(cs / 6000), (int)((cs / 100) % 60), (int)(cs % 100));
}

static void show_elapsed(void)
{
    if (s_time_lb == NULL) return;

    char buf[16];
    format_time(elapsed_us(), buf, sizeof(buf));
    lv_label_set_text(s_time_lb, buf);
}

static void show_state(void)
{
    if (s_state_lb != NULL) {
        lv_label_set_text(s_state_lb, s_running ? "计时中" : "已暂停");
        lv_obj_set_style_text_color(s_state_lb,
                                    s_running ? fw_theme_color_success()
                                              : fw_theme_color_text_secondary(), 0);
    }
    if (s_run_lb != NULL) lv_label_set_text(s_run_lb, s_running ? "暂停" : "开始");
    if (s_run_img != NULL) {
        lv_image_set_src(s_run_img, s_running ? &icon_ui_pause : &icon_ui_play);
    }
    if (s_lap_hint != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "已计次 %u 次（上限 %u 次）",
                 (unsigned)s_lap_count, (unsigned)LAP_MAX);
        lv_label_set_text(s_lap_hint, buf);
    }
}

/* 只删除列表条目，保留 fw_ui_list 的标题（标题是第 0 个子对象） */
static void list_clear_items(lv_obj_t *list)
{
    if (list == NULL) return;

    uint32_t n = lv_obj_get_child_count(list);
    for (uint32_t i = n; i > 1; i--) {
        lv_obj_delete(lv_obj_get_child(list, (int32_t)(i - 1)));
    }
}

/* 计次列表只增不重建：每次计次追加一行（左侧"第 N 次"、右侧本次分段用时），
 * 建完把这条滚到可见处 —— 100 次计次也不至于每次重排整表 */
static void append_lap(size_t idx)
{
    if (s_lap_list == NULL || s_laps == NULL || idx >= s_lap_count) return;

    if (idx == 0) list_clear_items(s_lap_list);     /* 去掉"还没有计次"提示行 */

    const int64_t prev = (idx == 0) ? 0 : s_laps[idx - 1];
    char t[16];
    char name[24];
    format_time(s_laps[idx] - prev, t, sizeof(t));
    snprintf(name, sizeof(name), "第 %u 次", (unsigned)(idx + 1));

    lv_obj_t *item = fw_ui_list_add_value(s_lap_list, name, t, NULL, NULL);
    if (item != NULL) lv_obj_scroll_to_view(item, LV_ANIM_OFF);
}

/* 复位 / 首次进入：清空列表并放回提示行 */
static void clear_laps(void)
{
    if (s_lap_list == NULL) return;

    list_clear_items(s_lap_list);
    fw_ui_list_hint(s_lap_list, "还没有计次");
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    if (s_running) show_elapsed();      /* 没在跑就不用动界面 */
}

static void lap_cb(lv_event_t *e)
{
    (void)e;

    if (!s_running) {
        fw_ui_toast("计时未开始", 2000);
        return;
    }
    if (s_laps == NULL) {
        fw_ui_toast("内存不足", 2000);
        return;
    }
    if (s_lap_count >= LAP_MAX) {
        fw_ui_toast("计次已满", 2000);
        return;
    }

    s_laps[s_lap_count++] = elapsed_us();
    append_lap(s_lap_count - 1);
    show_state();
}

static void run_cb(lv_event_t *e)
{
    (void)e;

    if (s_running) {
        s_accum_us = elapsed_us();
        s_running = false;
    } else {
        s_start_us = esp_timer_get_time();
        s_running = true;
    }
    show_state();
    show_elapsed();
}

static void reset_cb(lv_event_t *e)
{
    (void)e;

    s_running = false;
    s_accum_us = 0;
    s_lap_count = 0;            /* 复位清空计次（暂停时保留） */
    show_state();
    show_elapsed();
    clear_laps();
}

static void *stopwatch_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_laps == NULL) {
        s_laps = malloc(sizeof(int64_t) * LAP_MAX);
    }
    s_lap_count = 0;
    s_running = false;
    s_accum_us = 0;
    s_foreground = false;

    /* 主信息卡：用时是这一页的主角 */
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
    lv_label_set_text(s_time_lb, "00:00.00");
    lv_obj_set_style_text_font(s_time_lb, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_time_lb, fw_theme_color_text_primary(), 0);

    s_state_lb = lv_label_create(line1);
    lv_obj_set_style_text_font(s_state_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_state_lb, fw_theme_color_text_secondary(), 0);

    s_lap_hint = lv_label_create(card);
    lv_obj_set_width(s_lap_hint, lv_pct(100));
    lv_label_set_long_mode(s_lap_hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_lap_hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_lap_hint, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_lap_hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 计次列表（余下的高度都给它） */
    s_lap_list = fw_ui_list(body, "计次");
    lv_obj_set_width(s_lap_list, lv_pct(100));
    lv_obj_set_flex_grow(s_lap_list, 1);

    /* 底部：开始暂停 / 计次 / 复位 */
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
    fw_ui_action_btn(bar, &icon_ui_flag, "计次", 0, lap_cb, NULL);
    fw_ui_action_btn(bar, &icon_ui_refresh, "复位", 0, reset_cb, NULL);

    show_state();
    show_elapsed();
    clear_laps();

    /* 定时器先建好但不启用：等进入前台（on_start / on_resume）再恢复 */
    s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    if (s_timer != NULL) lv_timer_pause(s_timer);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* on_start 与 on_resume 都挂这个：换主题重建时后台 App 也会走 on_create，
 * 而暂停过的定时器在"首次进入"与"从返回栈回来"两条路径上都要恢复；
 * 框架在"从桌面重新进入"时两个都会调，用 s_foreground 去重 */
static void stopwatch_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    show_state();
    show_elapsed();
}

static void stopwatch_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);

    /* 真的离开 App（回桌面 / 返回上一级）就停止计时：换主题是前台原地重建，
     * is_foreground 仍为真，那种情况不打断。停下保留累计值，回来按「开始」接着走 */
    if (!fw_app_mgr_is_foreground("Stopwatch") && s_running) {
        s_accum_us = elapsed_us();
        s_running = false;
        show_state();
    }
}

static void stopwatch_on_destroy(void *ctx)
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
    s_time_lb = NULL;
    s_state_lb = NULL;
    s_lap_hint = NULL;
    s_run_img = NULL;
    s_run_lb = NULL;
    s_lap_list = NULL;
    lvgl_port_unlock();

    free(s_laps);
    s_laps = NULL;
    s_lap_count = 0;

    s_running = false;
    s_accum_us = 0;
}

const fw_app_desc_t app_stopwatch_desc = {
    .name = "Stopwatch",
    .title = "秒表",
    .icon = &icon_home_stopwatch,
    .on_create = stopwatch_on_create,
    .on_start = stopwatch_on_resume,
    .on_pause = stopwatch_on_pause,
    .on_resume = stopwatch_on_resume,
    .on_destroy = stopwatch_on_destroy,
};
