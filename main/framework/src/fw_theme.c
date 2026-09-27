/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Theme 实现
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.theme";

#define FW_THEME_NS    "sys"
#define FW_THEME_KEY   "theme"

typedef struct {
    uint32_t bg_primary;
    uint32_t bg_secondary;
    uint32_t bg_card;
    uint32_t text_primary;
    uint32_t text_secondary;
    uint32_t text_disabled;
    uint32_t accent;
    uint32_t accent2;
    uint32_t success;
    uint32_t warning;
    uint32_t error;
    uint32_t divider;
    uint32_t border;
} fw_palette_t;

static const fw_palette_t s_dark = {
    .bg_primary     = 0x0A0D12,   /* 页面底色：近黑，冷调，和卡片拉开层次 */
    .bg_secondary   = 0x151A22,   /* 状态栏 / 浮层 */
    .bg_card        = 0x1F2630,   /* 卡片：比底色亮两档 */
    .text_primary   = 0xFFFFFF,
    .text_secondary = 0xC4CDDB,
    .text_disabled  = 0x606A78,
    .accent         = 0x3D8CFF,   /* 主色：高饱和蓝，用在图标 / 卡片强调边 / 主按钮 */
    .accent2        = 0xFF6B3D,   /* 辅色：暖橙 */
    .success        = 0x4ADE80,
    .warning        = 0xFFC94D,
    .error          = 0xFF5A5C,
    .divider        = 0x2A323D,
    .border         = 0x333D4A,
};

static const fw_palette_t s_light = {
    .bg_primary     = 0xE4ECF7,   /* 页面底色：浅灰蓝，衬托白卡片 */
    .bg_secondary   = 0xFFFFFF,   /* 状态栏 / 浮层：纯白 */
    .bg_card        = 0xFFFFFF,   /* 卡片 */
    .text_primary   = 0x0F1319,
    .text_secondary = 0x4C5768,
    .text_disabled  = 0x98A3B2,
    .accent         = 0x0A5CD6,
    .accent2        = 0xE8551F,
    .success        = 0x16A34A,
    .warning        = 0xD97706,
    .error          = 0xDC2626,
    .divider        = 0xD6E0EC,
    .border         = 0xBCC9DA,
};

static fw_theme_t s_theme = FW_THEME_DARK;
static const fw_palette_t *s_pal = &s_dark;

/* LVGL 自带主题是单例：lv_theme_default_init() 只在参数变化时重设它的样式，
 * 不会每次新建对象，所以明暗切换直接按当前主题再 init 一次；
 * 想同时留两份（明 / 暗各一份）反而会共用同一份样式，切主题后颜色不跟着变 */
static void apply_lvgl_theme(void)
{
    lv_display_t *d = lv_display_get_default();
    if (d == NULL) return;

    lvgl_port_lock(0);
    lv_theme_t *th = lv_theme_default_init(d,
                                          lv_color_hex(s_pal->accent),
                                          lv_color_hex(s_pal->accent2),
                                          (s_theme == FW_THEME_DARK),
                                          fw_asset_font_14());
    lv_display_set_theme(d, th);
    lvgl_port_unlock();
}

esp_err_t fw_theme_apply(fw_theme_t theme)
{
    const fw_theme_t want = (theme == FW_THEME_LIGHT) ? FW_THEME_LIGHT : FW_THEME_DARK;

    /* 没变就别折腾：换主题会重建全部界面 */
    if (want == s_theme) return ESP_OK;

    s_theme = want;
    s_pal = (s_theme == FW_THEME_LIGHT) ? &s_light : &s_dark;

    apply_lvgl_theme();

    esp_err_t err = svc_settings_set_u8(FW_THEME_NS, FW_THEME_KEY, (uint8_t)s_theme);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save theme failed: %s", esp_err_to_name(err));
    }
    svc_event_bus_publish(SVC_EVENT_THEME_CHANGED, &s_theme, sizeof(s_theme));

    ESP_LOGI(TAG, "theme applied: %s", (s_theme == FW_THEME_LIGHT) ? "light" : "dark");
    return ESP_OK;
}

esp_err_t fw_theme_init(void)
{
    uint8_t t = FW_THEME_DARK;
    svc_settings_get_u8(FW_THEME_NS, FW_THEME_KEY, &t, FW_THEME_DARK);

    s_theme = (t == FW_THEME_LIGHT) ? FW_THEME_LIGHT : FW_THEME_DARK;
    s_pal = (s_theme == FW_THEME_LIGHT) ? &s_light : &s_dark;

    apply_lvgl_theme();

    ESP_LOGI(TAG, "initialized (%s)", (s_theme == FW_THEME_LIGHT) ? "light" : "dark");
    return ESP_OK;
}

fw_theme_t fw_theme_current(void)
{
    return s_theme;
}

lv_color_t fw_theme_color_bg_primary(void)     { return lv_color_hex(s_pal->bg_primary); }
lv_color_t fw_theme_color_bg_secondary(void)   { return lv_color_hex(s_pal->bg_secondary); }
lv_color_t fw_theme_color_bg_card(void)        { return lv_color_hex(s_pal->bg_card); }
lv_color_t fw_theme_color_text_primary(void)   { return lv_color_hex(s_pal->text_primary); }
lv_color_t fw_theme_color_text_secondary(void) { return lv_color_hex(s_pal->text_secondary); }
lv_color_t fw_theme_color_text_disabled(void)  { return lv_color_hex(s_pal->text_disabled); }
lv_color_t fw_theme_color_accent(void)         { return lv_color_hex(s_pal->accent); }
lv_color_t fw_theme_color_accent2(void)        { return lv_color_hex(s_pal->accent2); }
lv_color_t fw_theme_color_success(void)        { return lv_color_hex(s_pal->success); }
lv_color_t fw_theme_color_warning(void)        { return lv_color_hex(s_pal->warning); }
lv_color_t fw_theme_color_error(void)          { return lv_color_hex(s_pal->error); }
lv_color_t fw_theme_color_divider(void)        { return lv_color_hex(s_pal->divider); }
lv_color_t fw_theme_color_border(void)         { return lv_color_hex(s_pal->border); }
