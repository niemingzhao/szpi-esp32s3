/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Browser（APP-BROWSER 浏览器）
 *
 * 只做"取回网页文本"：输入 URL → svc_http_get_async() 在独立任务里抓取（不阻塞界面）
 * → 去掉 HTML 标签、解几个常见实体、压掉多余空白 → 显示在可滚动文本框里。
 * 不支持 JS、不支持 https（svc_http_get 没挂证书，https 会失败），要 https 用 OTA 那套。
 */

#include "app_browser.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.browser";

#define BR_MAX_BODY   6000

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_url = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_kb = NULL;

static char s_url_buf[128] = "http://neverssl.com";
static char *s_resp = NULL;          /* 6000 B 放 PSRAM（malloc），不占 BSS */
static bool s_busy = false;
static bool s_gone = false;          /* App 已销毁但抓取还在跑 */

/* ------------------------------ HTML 处理 ------------------------------ */

static void append_char(char *out, size_t len, size_t *pos, char c)
{
    if (*pos + 1 < len) {
        out[*pos] = c;
        *pos += 1;
        out[*pos] = '\0';
    }
}

/* 去标签 + 压空白 + 解常见实体（够看文本，不做真正的 HTML 渲染） */
static void html_to_text(const char *in, char *out, size_t len)
{
    size_t pos = 0;
    bool in_tag = false;
    bool last_space = true;

    out[0] = '\0';

    for (size_t i = 0; in[i] != '\0' && pos + 1 < len; i++) {
        char c = in[i];

        if (c == '<') {
            in_tag = true;
            /* <br> / <p> / <li> 之类当作换行 */
            if (strncasecmp(in + i, "<br", 3) == 0 || strncasecmp(in + i, "<p", 2) == 0 ||
                strncasecmp(in + i, "<li", 3) == 0 || strncasecmp(in + i, "<div", 4) == 0) {
                append_char(out, len, &pos, '\n');
                last_space = true;
            }
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (in_tag) continue;

        if (c == '&') {
            if (strncasecmp(in + i, "&amp;", 5) == 0) { append_char(out, len, &pos, '&'); i += 4; }
            else if (strncasecmp(in + i, "&lt;", 4) == 0) { append_char(out, len, &pos, '<'); i += 3; }
            else if (strncasecmp(in + i, "&gt;", 4) == 0) { append_char(out, len, &pos, '>'); i += 3; }
            else if (strncasecmp(in + i, "&nbsp;", 6) == 0) { append_char(out, len, &pos, ' '); i += 5; }
            else if (strncasecmp(in + i, "&quot;", 6) == 0) { append_char(out, len, &pos, '"'); i += 5; }
            else append_char(out, len, &pos, '&');
            last_space = false;
            continue;
        }

        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || c == ' ') {
            if (last_space) continue;
            append_char(out, len, &pos, ' ');
            last_space = true;
            continue;
        }

        append_char(out, len, &pos, c);
        last_space = false;
    }
}

/* ------------------------------ 抓取 ------------------------------ */

static void http_done_cb(const char *body, esp_err_t err, void *user)
{
    (void)user;

    if (s_gone) {                     /* App 已经关了：直接释放缓冲 */
        free(s_resp);
        s_resp = NULL;
        return;
    }

    lvgl_port_lock(0);
    s_busy = false;

    if (err != ESP_OK || body == NULL) {
        lv_label_set_text(s_status, "加载失败（只支持 http://，不支持 https）");
        lv_label_set_text(s_body, "");
    } else {
        /* 原地去标签：输出长度一定小于输入，不需要额外 6 KB 缓冲（服务任务的栈放不下） */
        html_to_text(body, (char *)body, BR_MAX_BODY);
        lv_label_set_text(s_body, body);
        lv_label_set_text(s_status, "加载完成");
    }

    lvgl_port_unlock();
}

static void open_cb(lv_event_t *e)
{
    (void)e;

    if (s_busy) return;

    svc_net_status_t st;
    if (svc_net_get_status(&st) != ESP_OK || !st.wifi_connected) {
        fw_ui_toast("Wi-Fi 未连接", 2000);
        return;
    }

    if (strncmp(s_url_buf, "http://", 7) != 0) {
        fw_ui_toast("请填 http:// 开头的地址", 2500);
        return;
    }

    if (s_resp == NULL) {
        fw_ui_toast("内存不足", 2000);
        return;
    }

    lvgl_port_lock(0);
    lv_label_set_text(s_status, "加载中…");
    lv_label_set_text(s_body, "");
    lvgl_port_unlock();

    if (svc_http_get_async(s_url_buf, s_resp, BR_MAX_BODY, http_done_cb, NULL) != ESP_OK) {
        lvgl_port_lock(0);
        lv_label_set_text(s_status, "请求启动失败");
        lvgl_port_unlock();
        return;
    }

    s_busy = true;
}

/* ------------------------------ 键盘 ------------------------------ */

static void keyboard_toggle(bool show)
{
    if (s_kb == NULL) return;

    lvgl_port_lock(0);
    if (show) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

static void kb_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_READY:
    case LV_EVENT_CANCEL:
        keyboard_toggle(false);
        break;
    default:
        break;
    }
}

static void ta_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        keyboard_toggle(true);
    }
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *browser_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 响应缓冲放 PSRAM（6 KB 不进 BSS），App 关闭时释放 */
    s_resp = malloc(BR_MAX_BODY);
    s_gone = false;

    s_url = lv_textarea_create(body);
    lv_obj_set_width(s_url, lv_pct(100));
    lv_obj_set_height(s_url, 38);
    lv_textarea_set_one_line(s_url, true);
    lv_textarea_set_max_length(s_url, 120);
    lv_textarea_set_text(s_url, s_url_buf);
    lv_obj_set_style_text_font(s_url, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_url, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_bg_color(s_url, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(s_url, 1, 0);
    lv_obj_set_style_border_color(s_url, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(s_url, 8, 0);
    lv_obj_add_event_cb(s_url, ta_event_cb, LV_EVENT_FOCUSED, NULL);

    fw_ui_row_btn(body, LV_SYMBOL_DOWNLOAD, "打开", open_cb, NULL);

    s_status = lv_label_create(body);
    lv_label_set_text(s_status, "只支持简单网页（无 JS / https）");
    lv_obj_set_style_text_font(s_status, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_status, fw_theme_color_text_secondary(), 0);

    /* 正文放在可滚动容器里，长文本自动换行 */
    lv_obj_t *scroll = lv_obj_create(body);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_bg_color(scroll, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(scroll, 1, 0);
    lv_obj_set_style_border_color(scroll, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(scroll, 8, 0);
    lv_obj_set_style_pad_all(scroll, 8, 0);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

    s_body = lv_label_create(scroll);
    lv_label_set_text(s_body, "");
    lv_obj_set_width(s_body, lv_pct(100));
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_body, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_body, fw_theme_color_text_primary(), 0);

    s_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_kb, lv_pct(100), 140);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_secondary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_card(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, fw_theme_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_kb, fw_theme_color_border(), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb, s_url);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void browser_on_destroy(void *ctx)
{
    (void)ctx;

    /* 抓取还在跑的话，缓冲交给 http_done_cb() 释放 */
    s_gone = true;
    if (!s_busy && s_resp != NULL) {
        free(s_resp);
        s_resp = NULL;
    }
    s_busy = false;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_url = NULL;
    s_status = NULL;
    s_body = NULL;
    s_kb = NULL;
    lvgl_port_unlock();
}

static bool browser_on_back(void *ctx)
{
    (void)ctx;

    if (s_kb != NULL && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) {
        keyboard_toggle(false);
        return true;
    }
    return false;
}

const fw_app_desc_t app_browser_desc = {
    .name = "Browser",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_DRIVE,
    .on_create = browser_on_create,
    .on_destroy = browser_on_destroy,
    .on_back = browser_on_back,
};
