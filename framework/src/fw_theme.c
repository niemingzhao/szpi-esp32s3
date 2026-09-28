/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Theme 实现
 */

#include "fw_common.h"
#include "svc_common.h"
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
    .bg_primary     = 0x121212,
    .bg_secondary   = 0x1E1E1E,
    .bg_card        = 0x2A2A2A,
    .text_primary   = 0xFFFFFF,
    .text_secondary = 0xBBBBBB,
    .text_disabled  = 0x666666,
    .accent         = 0x4F9EFF,
    .accent2        = 0x9C27B0,
    .success        = 0x4CAF50,
    .warning        = 0xFFC107,
    .error          = 0xF44336,
    .divider        = 0x2A2A2A,
    .border         = 0x333333,
};

static const fw_palette_t s_light = {
    .bg_primary     = 0xFFFFFF,
    .bg_secondary   = 0xF2F2F2,
    .bg_card        = 0xFFFFFF,
    .text_primary   = 0x1A1A1A,
    .text_secondary = 0x555555,
    .text_disabled  = 0xAAAAAA,
    .accent         = 0x1976D2,
    .accent2        = 0x7B1FA2,
    .success        = 0x388E3C,
    .warning        = 0xFFA000,
    .error          = 0xD32F2F,
    .divider        = 0xE0E0E0,
    .border         = 0xCCCCCC,
};

static fw_theme_t s_theme = FW_THEME_DARK;
static const fw_palette_t *s_pal = &s_dark;

esp_err_t fw_theme_apply(fw_theme_t theme)
{
    s_theme = (theme == FW_THEME_LIGHT) ? FW_THEME_LIGHT : FW_THEME_DARK;
    s_pal = (s_theme == FW_THEME_LIGHT) ? &s_light : &s_dark;

    svc_settings_set_u8(FW_THEME_NS, FW_THEME_KEY, (uint8_t)s_theme);
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
