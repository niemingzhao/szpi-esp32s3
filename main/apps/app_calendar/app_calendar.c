/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Calendar（APP-CALENDAR 日历）
 *
 * 单屏月视图：一行头部（《 年 / ◀ 月 / 标题 / ▶ 月 / 》 年 / 今天）+ 7 列网格。
 * 一大一小两层箭头分别切年和切月；整页 184 / 188，6 周也放得下，不用滚动。
 *
 * - 今天：强调色圆角底 + 白字；周末：次要色；相邻月份：置灰色且不可点。
 * - 长按本月的某一天 → 对话框「把 X 年 X 月 X 日设为今天？」→ 只改年月日、保留
 *   当前时分秒。改完这条时间就算"可信"（联网后 SNTP 会再校准回来），
 *   所以「日历手动设日期 + 时钟手动校时 + 联网自动同步」三件事凑齐了。
 * - 时间来自 svc_time；未同步（例如刚断电）时进页面弹一次 Toast 提示可长按设置。
 * - 每 60 s 检查一次"今天"是否变了（跨零点 / 改时区 / 手动改日期），变了就重画；
 *   定时器在后台暂停，进入前台（on_start 与 on_resume 都挂）恢复。
 */

#include "app_calendar.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "app.calendar";

#define WEEK_DAYS       7
#define WEEKS           6                  /* 固定 6 周，相邻月灰显，高度不跳 */
#define CELL_W          40
#define CELL_H          20
#define CELL_GAP_X      2                  /* 296 = 7×40 + 6×2 */
#define CELL_GAP_Y      1
#define HEADER_H        30
#define CAL_TICK_MS     60000              /* 每分钟看看"今天"变了没 */
#define YEAR_MIN        1970
#define YEAR_MAX        2099

static const char *WEEK[] = { "日", "一", "二", "三", "四", "五", "六" };

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_grid = NULL;
static lv_timer_t *s_timer = NULL;
static bool s_foreground = false;

/* 当前显示的月份 */
static int s_view_y = 1970;
static int s_view_m = 1;

static void set_view(int year, int month);

/* 今天（时间不可信时 s_has_today = false，不高亮） */
static bool s_has_today = false;
static int s_today_y = 1970;
static int s_today_m = 1;
static int s_today_d = 1;

/* -------------------------------- 日期计算 ------------------------------- */

static int days_in_month(int year, int month)
{
    static const uint8_t DIM[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    if (month < 1 || month > 12) return 30;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return DIM[month - 1];
}

/* 某月 1 号是周几（0 = 周日）。用 mktime + localtime，让夏令时自己判断 */
static int first_wday(int year, int month)
{
    struct tm tm = { 0 };
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;

    const time_t t = mktime(&tm);
    struct tm out;
    if (t == (time_t)-1 || localtime_r(&t, &out) == NULL) return 0;
    return out.tm_wday;
}

static void today_update(void)
{
    s_has_today = svc_time_is_synced();
    if (!s_has_today) return;

    struct tm tm;
    const time_t t = (time_t)svc_time_now();
    if (localtime_r(&t, &tm) == NULL) {
        s_has_today = false;
        return;
    }

    s_today_y = tm.tm_year + 1900;
    s_today_m = tm.tm_mon + 1;
    s_today_d = tm.tm_mday;
}

static bool is_today(int year, int month, int day)
{
    return s_has_today && year == s_today_y && month == s_today_m && day == s_today_d;
}

/* -------------------------------- 对话框 --------------------------------- */

static void date_dialog_cb(fw_dialog_btn_t btn, void *user);

static void day_long_cb(lv_event_t *e)
{
    const int day = (int)(intptr_t)lv_event_get_user_data(e);
    if (day < 1) return;

    char msg[96];
    snprintf(msg, sizeof(msg), "把 %d 年 %d 月 %d 日设为今天？", s_view_y, s_view_m, day);
    fw_ui_dialog(NULL, "设置日期", msg, FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 date_dialog_cb, (void *)(intptr_t)day);
}

/* 只改年月日，时分秒保留 */
static void date_dialog_cb(fw_dialog_btn_t btn, void *user)
{
    if (btn != FW_DIALOG_BTN_OK) return;

    const int day = (int)(intptr_t)user;

    struct tm tm;
    const time_t now = (time_t)svc_time_now();
    if (localtime_r(&now, &tm) == NULL) {
        fw_ui_toast("设置失败", 1500);
        return;
    }

    tm.tm_year = s_view_y - 1900;
    tm.tm_mon = s_view_m - 1;
    tm.tm_mday = day;
    tm.tm_isdst = -1;

    const time_t t = mktime(&tm);
    if (t == (time_t)-1 || svc_time_set_manual((int64_t)t) != ESP_OK) {
        fw_ui_toast("设置失败", 1500);
        return;
    }

    today_update();
    set_view(s_view_y, s_view_m);       /* 重画，让高亮跟到新日期 */
    fw_ui_toast("日期已更新", 1500);
}

/* -------------------------------- 月历网格 ------------------------------- */

static lv_obj_t *make_cell(lv_obj_t *parent)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_obj_set_size(lb, CELL_W, CELL_H);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    /* 先给个默认色，各分支再按状态覆盖（不能依赖 LVGL 自带主题） */
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    return lb;
}

static void build_month(void)
{
    if (s_grid == NULL) return;

    lv_obj_clean(s_grid);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d 年 %d 月", s_view_y, s_view_m);
    if (s_title != NULL) lv_label_set_text(s_title, buf);

    const int lead = first_wday(s_view_y, s_view_m);
    const int dim = days_in_month(s_view_y, s_view_m);
    const int prev_dim = days_in_month((s_view_m == 1) ? s_view_y - 1 : s_view_y,
                                       (s_view_m == 1) ? 12 : s_view_m - 1);

    /* 星期表头 */
    for (int i = 0; i < WEEK_DAYS; i++) {
        lv_obj_t *lb = make_cell(s_grid);
        lv_label_set_text(lb, WEEK[i]);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_secondary(), 0);
    }

    for (int i = 0; i < WEEK_DAYS * WEEKS; i++) {
        const int day = i - lead + 1;       /* <= 0 是上个月，> dim 是下个月 */
        const bool in_month = (day >= 1 && day <= dim);

        lv_obj_t *lb = make_cell(s_grid);

        if (!in_month) {
            snprintf(buf, sizeof(buf), "%d", (day <= 0) ? (prev_dim + day) : (day - dim));
            lv_label_set_text(lb, buf);
            lv_obj_set_style_text_color(lb, fw_theme_color_text_disabled(), 0);
            continue;
        }

        snprintf(buf, sizeof(buf), "%d", day);
        lv_label_set_text(lb, buf);

        if (is_today(s_view_y, s_view_m, day)) {
            lv_obj_set_style_bg_color(lb, fw_theme_color_accent(), 0);
            lv_obj_set_style_bg_opa(lb, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(lb, 8, 0);
            lv_obj_set_style_text_color(lb, lv_color_white(), 0);
            continue;                       /* 已经是今天了，不用再长按设置 */
        }

        lv_obj_set_style_text_color(lb,
                                    ((i % WEEK_DAYS) == 0 || (i % WEEK_DAYS) == 6)
                                        ? fw_theme_color_text_secondary()
                                        : fw_theme_color_text_primary(),
                                    0);

        /* 长按设为今天 */
        lv_obj_set_clickable(lb, true);
        lv_obj_add_event_cb(lb, day_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)day);
    }
}

/* -------------------------------- 事件回调 ------------------------------- */

static void shift_month_cb(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    set_view(s_view_y, s_view_m + delta);
}

static void shift_year_cb(lv_event_t *e)
{
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    set_view(s_view_y + delta, s_view_m);
}

static void today_cb(lv_event_t *e)
{
    (void)e;
    today_update();
    set_view(s_today_y, s_today_m);
}

/* 每分钟看一眼"今天"有没有变（跨零点 / 改时区 / 手动改过日期） */
static void tick_cb(lv_timer_t *t)
{
    (void)t;

    const int oy = s_today_y;
    const int om = s_today_m;
    const bool oh = s_has_today;

    today_update();

    /* 只有"显示的月份里可能出现或刚消失一个今天"时才重画 */
    const bool view_has = (s_view_y == s_today_y && s_view_m == s_today_m);
    const bool view_had = oh && (s_view_y == oy && s_view_m == om);
    if (view_has || view_had || oh != s_has_today) {
        build_month();
    }
}

/* -------------------------------- 页面搭建 ------------------------------- */

static lv_obj_t *make_nav_btn(lv_obj_t *parent, const char *text, lv_coord_t w,
                              lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, HEADER_H - 2);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 4);        /* 视觉不变，触摸区向外扩 4 px */
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
    return btn;
}

static void set_view(int year, int month)
{
    while (month < 1) {
        month += 12;
        year--;
    }
    while (month > 12) {
        month -= 12;
        year++;
    }
    if (year < YEAR_MIN) {
        year = YEAR_MIN;
        month = 1;
    } else if (year > YEAR_MAX) {
        year = YEAR_MAX;
        month = 12;
    }

    s_view_y = year;
    s_view_m = month;
    build_month();
}

/* ------------------------------- 生命周期 ------------------------------- */

static void *calendar_on_create(void)
{
    lvgl_port_lock(0);

    s_foreground = false;

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 这一页刚好一屏（184 / 188），不需要滚动；顺手关掉，免得按住某天时
     * 被 LVGL 当成"拖动滚动"而取消长按 */
    lv_obj_set_scrollable(body, false);

    today_update();

    /* 头部：一行放下 —— 《 年 / ◀ 月 / 标题 / ▶ 月 / 》 年 / 今天 */
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), HEADER_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    fw_ui_icon_btn(bar, &icon_ui_left2, NULL, 30, shift_year_cb, (void *)(intptr_t)-1);
    fw_ui_icon_btn(bar, &icon_ui_left, NULL, 30, shift_month_cb, (void *)(intptr_t)-1);

    s_title = lv_label_create(bar);
    lv_obj_set_flex_grow(s_title, 1);            /* 中间的空白都给标题，文字居中 */
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_title, fw_theme_color_text_primary(), 0);

    fw_ui_icon_btn(bar, &icon_ui_right, NULL, 30, shift_month_cb, (void *)(intptr_t)1);
    fw_ui_icon_btn(bar, &icon_ui_right2, NULL, 30, shift_year_cb, (void *)(intptr_t)1);
    make_nav_btn(bar, "今天", 36, today_cb, NULL);

    /* 网格：7 列 ×（表头 + 6 周），列距 2、行距 1：7×40 + 6×2 = 292，装得下 296 */
    s_grid = lv_obj_create(body);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_height(s_grid, (WEEKS + 1) * CELL_H + WEEKS * CELL_GAP_Y);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_set_style_pad_column(s_grid, CELL_GAP_X, 0);
    lv_obj_set_style_pad_row(s_grid, CELL_GAP_Y, 0);
    lv_obj_set_scrollable(s_grid, false);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    set_view(s_today_y, s_today_m);              /* 内部会 build_month() */

    if (!s_has_today) {
        fw_ui_toast("时间未同步，可长按某天设置", 3000);
    }

    /* 每分钟查一次"今天"；先暂停，进入前台再由 on_start / on_resume 打开 */
    s_timer = lv_timer_create(tick_cb, CAL_TICK_MS, NULL);
    if (s_timer != NULL) {
        lv_timer_pause(s_timer);
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (view=%d-%d today=%d-%d-%d synced=%d)",
             s_view_y, s_view_m, s_today_y, s_today_m, s_today_d, s_has_today ? 1 : 0);
    return s_root;
}

static void calendar_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

/* on_start 与 on_resume 都挂这个：从桌面点进来走 on_start，从返回栈回来走 on_resume */
static void calendar_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);

    /* 每次回到前台都跳回当前月：上次翻到的月份不留着（用户要求：进来就是当月） */
    today_update();
    set_view(s_today_y, s_today_m);
}

static void calendar_on_destroy(void *ctx)
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
    s_title = NULL;
    s_grid = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_calendar_desc = {
    .name = "Calendar",
    .title = "日历",
    .icon_64 = &icon_home_calendar,
    .symbol = LV_SYMBOL_LIST,
    .on_create = calendar_on_create,
    .on_start = calendar_on_resume,
    .on_pause = calendar_on_pause,
    .on_resume = calendar_on_resume,
    .on_destroy = calendar_on_destroy,
};
