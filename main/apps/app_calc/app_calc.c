/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Calculator（计算器）
 *
 * 四则运算 + 括号，自己写了个很小的递归下降求值器（不引第三方库）。
 * 上面一行是最近一次算完的式子，下面一行是当前输入 / 结果；键盘 4 × 5：
 *   C  ⌫  (  )
 *   7  8  9  ÷
 *   4  5  6  ×
 *   1  2  3  -
 *   0  .  +  =
 *
 * 屏幕就这么高（内容区 188 px）：显示区固定 44 px 放两行小字，剩下全给键盘
 * （键 71 × 24、5 行、行距 4 = 136 px），一屏刚好放下、不滚动，所以显示区不做卡片
 * （卡片内边距 10，两行字放不进去）。
 *
 * 算完接着按运算符是在这个结果上继续算（12+3= 之后再按 +5= 得 20），按数字 / 小数点 /
 * 括号才是开一条新式子。÷ 0、括号不配对于显示「错误」。
 */

#include "app_calc.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.calc";

#define CALC_EXPR_MAX 64        /* 输入缓冲（含结尾），够放长式子 */
#define CALC_DISP_H   44        /* 显示区：历史一行 + 当前行 */
#define CALC_KEY_W    71        /* 4 × 71 + 3 × 4 = 296 = 内容区宽度 */
#define CALC_KEY_H    24
#define CALC_KEY_GAP  4

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_history = NULL;      /* 上一次算完的式子 */
static lv_obj_t *s_expr = NULL;         /* 当前输入 / 结果 */

static char s_input[CALC_EXPR_MAX] = "";
static bool s_showing_result = false;   /* 当前显示的是刚算出的结果 */

/* ------------------------------ 求值器 ------------------------------ */

static const char *s_p = NULL;

static void skip_spaces(void)
{
    while (*s_p == ' ') s_p++;
}

static double parse_expr(void);

static double parse_factor(void)
{
    skip_spaces();

    if (*s_p == '(') {
        s_p++;
        double v = parse_expr();
        skip_spaces();
        if (*s_p == ')') s_p++;
        return v;
    }

    if (*s_p == '-') {          /* 一元负号 */
        s_p++;
        return -parse_factor();
    }

    char *end = NULL;
    double v = strtod(s_p, &end);
    if (end == s_p) return NAN;
    s_p = end;
    return v;
}

static double parse_term(void)
{
    double v = parse_factor();

    for (;;) {
        skip_spaces();
        if (*s_p == '*') {
            s_p++;
            v *= parse_factor();
        } else if (*s_p == '/') {
            s_p++;
            double d = parse_factor();
            v = (d == 0.0) ? NAN : (v / d);
        } else {
            return v;
        }
    }
}

static double parse_expr(void)
{
    double v = parse_term();

    for (;;) {
        skip_spaces();
        if (*s_p == '+') {
            s_p++;
            v += parse_term();
        } else if (*s_p == '-') {
            s_p++;
            v -= parse_term();
        } else {
            return v;
        }
    }
}

/* 把 UTF-8 的 × ÷ 换成 ASCII，便于求值（显示时仍是 × ÷） */
static void to_ascii(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < len; ) {
        if (strncmp(in + i, "\xC3\x97", 2) == 0) {          /* × */
            out[o++] = '*';
            i += 2;
        } else if (strncmp(in + i, "\xC3\xB7", 2) == 0) {   /* ÷ */
            out[o++] = '/';
            i += 2;
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

static esp_err_t evaluate(const char *expr, double *out)
{
    char ascii[CALC_EXPR_MAX * 2];
    to_ascii(expr, ascii, sizeof(ascii));

    s_p = ascii;
    double v = parse_expr();
    skip_spaces();

    if (*s_p != '\0' || isnan(v)) return ESP_FAIL;

    *out = v;
    return ESP_OK;
}

/* ------------------------------- 界面 ------------------------------- */

static void refresh_display(void)
{
    if (s_expr == NULL) return;

    lv_label_set_text(s_expr, (s_input[0] != '\0') ? s_input : "0");
}

/* 刚算完结果时接着敲运算符（+ - × ÷）是在这个结果上继续算；
 * 敲数字 / 小数点 / 括号（结果后面接括号是非法式子）则重开一条 */
static bool continues_result(const char *text)
{
    return !((text[0] >= '0' && text[0] <= '9') ||
             text[0] == '.' || text[0] == '(' || text[0] == ')');
}

static void append(const char *text)
{
    if (s_showing_result) {
        if (!continues_result(text)) s_input[0] = '\0';
        s_showing_result = false;
    }

    if (strlen(s_input) + strlen(text) >= CALC_EXPR_MAX) return;

    strlcat(s_input, text, sizeof(s_input));
    refresh_display();
}

static void key_cb(lv_event_t *e)
{
    const char *text = (const char *)lv_event_get_user_data(e);
    if (text == NULL) return;

    if (strcmp(text, "C") == 0) {
        s_input[0] = '\0';
        s_showing_result = false;
        if (s_history != NULL) lv_label_set_text(s_history, "");
        refresh_display();
        return;
    }

    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        size_t len = strlen(s_input);
        if (len > 0) {
            /* 退一步时不要把多字节字符切一半 */
            do {
                len--;
            } while (len > 0 && ((unsigned char)s_input[len] & 0xC0) == 0x80);
            s_input[len] = '\0';
        }
        s_showing_result = false;
        refresh_display();
        return;
    }

    if (strcmp(text, "=") == 0) {
        if (s_input[0] == '\0' || s_showing_result) return;

        double v;
        if (evaluate(s_input, &v) != ESP_OK) {
            if (s_history != NULL) lv_label_set_text(s_history, s_input);
            lv_label_set_text(s_expr, "错误");
            s_input[0] = '\0';
            s_showing_result = true;
            return;
        }

        char result[32];
        snprintf(result, sizeof(result), "%.10g", v);

        if (s_history != NULL) {
            char line[CALC_EXPR_MAX + 8];
            snprintf(line, sizeof(line), "%s =", s_input);
            lv_label_set_text(s_history, line);
        }

        strlcpy(s_input, result, sizeof(s_input));
        refresh_display();
        s_showing_result = true;
        ESP_LOGI(TAG, "= %s", result);
        return;
    }

    append(text);
}

/* ------------------------------ 生命周期 ------------------------------ */

/* 一个键：卡片底 + 描边；"=" 用主色底（文字颜色必须显式给，不能依赖自带主题） */
static void make_key(lv_obj_t *parent, const char *text, bool accent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, CALC_KEY_W, CALC_KEY_H);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, accent ? fw_theme_color_accent() : fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(btn, key_cb, LV_EVENT_SHORT_CLICKED, (void *)text);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(label,
                                accent ? fw_theme_color_bg_primary()
                                       : fw_theme_color_text_primary(), 0);
    lv_obj_center(label);
}

static void *calc_on_create(void)
{
    lvgl_port_lock(0);

    /* 每次重新创建都从空式子开始（换主题重建也会走这里，别留着上一位用户的状态）；
     * 显示区默认显示 0，所以创建时也要刷一次 */
    s_input[0] = '\0';
    s_showing_result = false;

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);
    lv_obj_set_scrollable(body, false);     /* 一屏刚好放下，别让它能滚 */

    /* 显示区：上面一行历史（次要色小字），下面一行当前输入 / 结果（大一点）。
     * 不做卡片：卡片内边距 10，两行字塞不进剩下的那点高度 */
    lv_obj_t *disp = lv_obj_create(body);
    lv_obj_set_size(disp, lv_pct(100), CALC_DISP_H);
    lv_obj_set_scrollable(disp, false);
    lv_obj_set_style_bg_opa(disp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(disp, 0, 0);
    lv_obj_set_style_pad_all(disp, 0, 0);

    s_history = lv_label_create(disp);
    lv_label_set_text(s_history, "");
    lv_obj_set_style_text_font(s_history, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_history, fw_theme_color_text_secondary(), 0);
    lv_label_set_long_mode(s_history, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_history, lv_pct(100));
    lv_obj_set_style_text_align(s_history, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_history, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_expr = lv_label_create(disp);
    lv_label_set_text(s_expr, "0");
    lv_obj_set_style_text_font(s_expr, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(s_expr, fw_theme_color_text_primary(), 0);
    /* 式子可能比一屏宽：让它自己滚动，至少能看全 */
    lv_label_set_long_mode(s_expr, LV_LABEL_LONG_MODE_SCROLL);
    lv_obj_set_width(s_expr, lv_pct(100));
    lv_obj_set_style_text_align(s_expr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_expr, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    /* 键盘：余下的高度全给它，键固定 71 × 24、每行 4 个 */
    static const char *const k_rows[5][4] = {
        { "C", LV_SYMBOL_BACKSPACE, "(", ")" },
        { "7", "8", "9", "\xC3\xB7" },
        { "4", "5", "6", "\xC3\x97" },
        { "1", "2", "3", "-" },
        { "0", ".", "+", "=" },
    };

    lv_obj_t *pad = lv_obj_create(body);
    lv_obj_set_width(pad, lv_pct(100));
    lv_obj_set_flex_grow(pad, 1);
    lv_obj_set_scrollable(pad, false);
    lv_obj_set_style_bg_opa(pad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pad, 0, 0);
    lv_obj_set_style_pad_all(pad, 0, 0);
    lv_obj_set_style_pad_row(pad, CALC_KEY_GAP, 0);
    lv_obj_set_style_pad_column(pad, CALC_KEY_GAP, 0);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    /* 塞不满一行时左对齐，不满一屏时靠上（网格必须左对齐） */
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (size_t r = 0; r < 5; r++) {
        for (size_t c = 0; c < 4; c++) {
            const char *t = k_rows[r][c];
            make_key(pad, t, strcmp(t, "=") == 0);
        }
    }

    refresh_display();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void calc_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_history = NULL;
    s_expr = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_calc_desc = {
    .name = "Calculator",
    .title = "计算器",
    .icon = &icon_home_calc,
    .on_create = calc_on_create,
    .on_destroy = calc_on_destroy,
};
