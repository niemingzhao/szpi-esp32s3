/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Log（系统日志）
 *
 * 顶部一行是"过滤 + 自动刷新 + 崩溃记录 + 清除"，下面一整块滚动区显示日志正文。
 * 过滤按钮循环切换：全部 → 错误+警告 → 仅错误 → 之后是在当前日志里出现过的 TAG
 * （按 TAG 看某个服务的输出）→ 回到全部。标签从行首解析（E (123) svc.audio: …）。
 *
 * 「自动刷新」关掉后每 3 s 的刷新停住，方便定住看；「崩溃记录」把滚动区切换成上次崩溃
 * 现场（此时自动刷新也停住，免得被冲掉），再看一次回到日志，此时才显示「清除」按钮。
 *
 * 显示整环（2 KB）：日志一行几十字节，只取尾部 512 B 只能看到几行。刷新时**贴着底部
 * 才会跟着往下滚**，用户翻上去看历史时不会被新日志顶走；内容与上次完全一致时直接跳过
 * （超大自动换行标签的重排不便宜，别每 3 s 白做一次）。
 */

#include "app_log.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.log";

#define LOG_SHOW_MAX    2048    /* 与服务里日志环的大小一致：整环都显示出来 */
#define REFRESH_MS      2000
#define TAG_MAX         8       /* 过滤里最多列几个 TAG */
#define TAG_NAME_MAX    24

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_log_box = NULL;
static lv_obj_t *s_log_lb = NULL;
static lv_obj_t *s_filter_btn = NULL;
static lv_obj_t *s_filter_lb = NULL;
static lv_obj_t *s_auto_img = NULL;
static lv_obj_t *s_clear_btn = NULL;
static lv_timer_t *s_timer = NULL;

/* 常驻缓冲（on_create 分配、on_destroy 释放）：2 KB 放栈上太占 LVGL 任务 */
static char *s_raw = NULL;      /* 日志原文 */
static char *s_out = NULL;      /* 过滤后的文本 */
static char *s_prev = NULL;     /* 上次显示的内容，用来跳过没变化的刷新 */
static lv_timer_t *s_follow_timer = NULL;   /* 一帧后再对一次底部（首帧容器高度可能还没算好） */

static bool s_foreground = false;
static bool s_auto = true;          /* 自动刷新 */
static bool s_crash = false;        /* 滚动区显示的是崩溃记录 */

static uint8_t s_filter = 0;        /* 0 = 全部，1 = 错误+警告，2 = 仅错误，>=3 = 按 TAG */
static char s_tags[TAG_MAX][TAG_NAME_MAX];
static uint8_t s_tag_count = 0;

/* ------------------------------- 解析 ------------------------------- */

/* 从一行日志里取 TAG：E (123) svc.audio: 消息 —— 取 ") " 到 ':' 之间那段。
 * 只在行首附近找，免得消息正文里的 ") " 被当成分隔 */
static bool line_tag(const char *line, char *out, size_t len)
{
    const char *p = line;
    const char *end = line + 24;
    while (p < end && *p != '\0') {
        if (p[0] == ')' && p[1] == ' ') break;
        p++;
    }
    if (p >= end || *p == '\0') return false;

    p += 2;
    const char *e = strchr(p, ':');
    if (e == NULL || e == p) return false;

    size_t n = (size_t)(e - p);
    if (n >= len) n = len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* 记住当前日志里出现过的 TAG（供过滤循环用），返回个数 */
static uint8_t collect_tags(const char *raw)
{
    s_tag_count = 0;

    const char *p = raw;
    while (*p != '\0' && s_tag_count < TAG_MAX) {
        char tag[TAG_NAME_MAX];
        if (line_tag(p, tag, sizeof(tag))) {
            bool dup = false;
            for (uint8_t i = 0; i < s_tag_count; i++) {
                if (strcmp(s_tags[i], tag) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) strlcpy(s_tags[s_tag_count++], tag, TAG_NAME_MAX);
        }

        const char *nl = strchr(p, '\n');
        if (nl == NULL) break;
        p = nl + 1;
    }

    return s_tag_count;
}

/* 行是否命中当前过滤（级别按行首字符，TAG 按解析出来的名字） */
static bool line_matches(const char *line, size_t len)
{
    if (s_filter == 0) return true;

    if (s_filter <= 2) {
        if (len < 2 || line[1] != ' ') return false;
        if (s_filter == 2) return line[0] == 'E';
        return line[0] == 'E' || line[0] == 'W';
    }

    const uint8_t idx = (uint8_t)(s_filter - 3);
    if (idx >= s_tag_count) return false;

    char tag[TAG_NAME_MAX];
    return line_tag(line, tag, sizeof(tag)) && strcmp(tag, s_tags[idx]) == 0;
}

/* ------------------------------- 显示 ------------------------------- */

static void show_filter_text(void)
{
    if (s_filter_lb == NULL) return;

    char buf[TAG_NAME_MAX + 12];
    if (s_filter == 0) snprintf(buf, sizeof(buf), "全部");
    else if (s_filter == 1) snprintf(buf, sizeof(buf), "错误+警告");
    else if (s_filter == 2) snprintf(buf, sizeof(buf), "仅错误");
    else {
        const uint8_t idx = (uint8_t)(s_filter - 3);
        snprintf(buf, sizeof(buf), "标签 %s", (idx < s_tag_count) ? s_tags[idx] : "?");
    }
    lv_label_set_text(s_filter_lb, buf);
}

static void show_auto(void)
{
    if (s_auto_img != NULL) {
        lv_image_set_src(s_auto_img, s_auto ? &icon_ui_pause : &icon_ui_play);
    }
}

/* 滚到最底：先让 LVGL 把新文本的高度算出来再滚 */
static void follow_now(void)
{
    if (s_log_box == NULL) return;

    if (s_log_lb != NULL) lv_obj_update_layout(s_log_lb);
    lv_obj_scroll_to_y(s_log_box, LV_COORD_MAX, LV_ANIM_OFF);
}

/* 一帧之后再对一次：首次显示时内容区的 flex 高度可能还没算出来，那一次滚动会滚不到底，
 * 而"是否贴底"的判断随即变假，之后就再也不跟随了 */
static void follow_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_follow_timer = NULL;          /* 一次性定时器，LVGL 自己删 */
    follow_now();
}

static void show_body(const char *text)
{
    if (s_log_lb == NULL || text == NULL) return;
    if (s_prev != NULL && strcmp(s_prev, text) == 0) return;    /* 内容没变：不重排 */

    /* 只有本来就贴着底部才跟着往下滚：翻上去看历史时别被新日志顶走 */
    const bool at_bottom = (s_log_box == NULL) || (lv_obj_get_scroll_bottom(s_log_box) <= 2);

    lv_label_set_text(s_log_lb, text);

    if (s_prev != NULL) strlcpy(s_prev, text, LOG_SHOW_MAX + 1);

    if (at_bottom) {
        follow_now();
        if (s_follow_timer == NULL) {
            s_follow_timer = lv_timer_create(follow_timer_cb, 60, NULL);
            if (s_follow_timer != NULL) lv_timer_set_repeat_count(s_follow_timer, 1);
        }
    }
}

static void render_log(void)
{
    if (s_raw == NULL) return;

    if (svc_sysinfo_get_recent_logs(s_raw, LOG_SHOW_MAX + 1) != ESP_OK) {
        show_body("(暂无日志)");
        return;
    }

    /* 环里存的是"最后 N 字节"，开头可能截在半行上：从第一个换行之后开始显示 */
    const char *p = strchr(s_raw, '\n');
    p = (p != NULL) ? (p + 1) : s_raw;

    collect_tags(p);

    if (s_filter == 0 || s_out == NULL) {
        show_body(p);
        return;
    }

    size_t o = 0;
    char *out = s_out;

    while (*p != '\0' && o + 1 < (LOG_SHOW_MAX + 1)) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);

        if (line_matches(p, len)) {
            size_t copy = len;
            if (copy > LOG_SHOW_MAX - o) copy = LOG_SHOW_MAX - o;
            memcpy(out + o, p, copy);
            o += copy;
            if (o + 1 < (LOG_SHOW_MAX + 1)) out[o++] = '\n';
        }

        if (nl == NULL) break;
        p = nl + 1;
    }
    out[o] = '\0';

    show_body((o > 0) ? out : "(没有匹配的日志)");
}

static void render(void)
{
    if (s_crash) return;            /* 崩溃记录是静态的，别被定时刷新冲掉 */
    render_log();
}

/* ------------------------------- 事件 ------------------------------- */

static void filter_cb(lv_event_t *e)
{
    (void)e;

    /* 循环：全部 → 错误+警告 → 仅错误 → 各个 TAG → 回到全部 */
    const uint8_t total = (uint8_t)(3 + s_tag_count);
    s_filter = (uint8_t)((s_filter + 1) % total);

    show_filter_text();
    render();                   /* 只看日志页：正开着崩溃记录时别把它的内容顶掉 */
}

static void auto_cb(lv_event_t *e)
{
    (void)e;

    s_auto = !s_auto;
    show_auto();

    if (s_timer != NULL) {
        if (s_auto && s_foreground && !s_crash) lv_timer_resume(s_timer);
        else lv_timer_pause(s_timer);
    }
    if (s_auto) render();
}

static void crash_cb(lv_event_t *e)
{
    (void)e;

    s_crash = !s_crash;

    if (s_crash) {
        /* 崩溃记录写进 s_out（滚动区专用暂存；看崩溃记录时不会走 render_log） */
        if (s_out != NULL && svc_sysinfo_get_crash_log(s_out, LOG_SHOW_MAX + 1) == ESP_OK) {
            show_body(s_out);
        } else {
            show_body("(没有崩溃记录)");
        }
    } else {
        render_log();
    }

    if (s_clear_btn != NULL) lv_obj_set_hidden(s_clear_btn, !s_crash);
    if (s_timer != NULL) {
        if (s_auto && s_foreground && !s_crash) lv_timer_resume(s_timer);
        else lv_timer_pause(s_timer);
    }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;

    svc_sysinfo_clear_crash_log();
    fw_ui_toast("已清除崩溃记录", 2000);
    show_body("(没有崩溃记录)");
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    render();
}

/* ------------------------------- 生命周期 ------------------------------- */

static void *log_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);
    lv_obj_set_scrollable(body, false);

    s_foreground = false;
    s_auto = true;
    s_crash = false;
    s_filter = 0;
    s_tag_count = 0;

    if (s_raw == NULL) s_raw = malloc(LOG_SHOW_MAX + 1);
    if (s_out == NULL) s_out = malloc(LOG_SHOW_MAX + 1);
    if (s_prev == NULL) s_prev = malloc(LOG_SHOW_MAX + 1);
    if (s_prev != NULL) s_prev[0] = '\0';       /* 新标签：第一次一定要写进去 */

    /* 顶部一行：过滤（撑满）+ 自动刷新 + 崩溃记录 + 清除 */
    lv_obj_t *head = lv_obj_create(body);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_filter_btn = fw_ui_icon_btn(head, &icon_ui_filter, "全部", 0, filter_cb, NULL);
    s_filter_lb = lv_obj_get_child(s_filter_btn, 1);

    lv_obj_t *auto_btn = fw_ui_icon_btn(head, &icon_ui_pause, NULL, 36, auto_cb, NULL);
    s_auto_img = lv_obj_get_child(auto_btn, 0);

    fw_ui_icon_btn(head, &icon_ui_warning, NULL, 36, crash_cb, NULL);

    s_clear_btn = fw_ui_icon_btn(head, &icon_ui_trash, NULL, 36, clear_cb, NULL);
    lv_obj_set_hidden(s_clear_btn, true);       /* 只在看崩溃记录时给清除入口 */

    /* 日志正文：固定高度余量全给它，自己滚动（长日志不带动整页） */
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_style_bg_color(box, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    s_log_box = box;

    s_log_lb = lv_label_create(box);
    lv_obj_set_width(s_log_lb, lv_pct(100));
    lv_label_set_long_mode(s_log_lb, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_log_lb, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_log_lb, fw_theme_color_text_secondary(), 0);
    lv_label_set_text(s_log_lb, "(读取中)");

    show_filter_text();
    show_auto();
    render_log();

    /* 定时器先建好但不启用：等进入前台（on_start / on_resume）再恢复 */
    s_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
    if (s_timer != NULL) lv_timer_pause(s_timer);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* on_start 与 on_resume 都挂这个：换主题重建时后台 App 也会走 on_create，
 * 而暂停过的定时器在"首次进入"与"从返回栈回来"两条路径上都要恢复 */
static void log_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL && s_auto && !s_crash) lv_timer_resume(s_timer);
    render();
}

static void log_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void log_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_follow_timer != NULL) {
        lv_timer_delete(s_follow_timer);
        s_follow_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_log_box = NULL;
    s_log_lb = NULL;
    s_filter_btn = NULL;
    s_filter_lb = NULL;
    s_auto_img = NULL;
    s_clear_btn = NULL;
    lvgl_port_unlock();

    free(s_raw);
    s_raw = NULL;
    free(s_out);
    s_out = NULL;
    free(s_prev);
    s_prev = NULL;

    s_crash = false;
    s_tag_count = 0;
}

const fw_app_desc_t app_log_desc = {
    .name = "Log",
    .title = "系统日志",
    .icon = &icon_home_log,
    .on_create = log_on_create,
    .on_start = log_on_resume,
    .on_pause = log_on_pause,
    .on_resume = log_on_resume,
    .on_destroy = log_on_destroy,
};
