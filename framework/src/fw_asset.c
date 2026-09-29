/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Asset 实现
 *
 * 中文用 Noto Sans SC 14 / 16 px 子集（回退 font_cn_extra），拉丁用 Montserrat；
 * 图标优先 LVGL 内置符号（App 名称到符号的映射见 fw_asset_symbol_for()）。
 */

#include "fw_common.h"
#include "esp_log.h"
#include <string.h>

LV_FONT_DECLARE(font_cn14);
LV_FONT_DECLARE(font_cn16);

static const char *TAG = "fw.asset";

typedef struct {
    const char *name;
    const char *symbol;
} fw_asset_icon_t;

static const fw_asset_icon_t s_icons[] = {
    { "Home",       LV_SYMBOL_HOME },
    { "Clock",      LV_SYMBOL_BELL },
    { "Settings",   LV_SYMBOL_SETTINGS },
    { "Music",      LV_SYMBOL_AUDIO },
    { "Recorder",   LV_SYMBOL_EDIT },
    { "Camera",     LV_SYMBOL_IMAGE },
    { "Video",      LV_SYMBOL_VIDEO },
    { "File",       LV_SYMBOL_DIRECTORY },
    { "Editor",     LV_SYMBOL_EDIT },
    { "Calculator", LV_SYMBOL_LIST },
    { "IMU",        LV_SYMBOL_GPS },
    { "BLE",        LV_SYMBOL_BLUETOOTH },
    { "Browser",    LV_SYMBOL_DRIVE },
    { "OTA",        LV_SYMBOL_DOWNLOAD },
    { "Debug",      LV_SYMBOL_WARNING },
    { "About",      LV_SYMBOL_FILE },
    { "Factory",    LV_SYMBOL_SETTINGS },
};

const lv_font_t *fw_asset_font_14(void)
{
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *fw_asset_font_20(void)
{
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *fw_asset_font_24(void)
{
#if LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *fw_asset_font_32(void)
{
#if LV_FONT_MONTSERRAT_32
    return &lv_font_montserrat_32;
#else
    return fw_asset_font_24();
#endif
}

const lv_font_t *fw_asset_font_cn(void)
{
    return &font_cn14;
}

const lv_font_t *fw_asset_font_cn_large(void)
{
    return &font_cn16;
}

const char *fw_asset_symbol_for(const char *app_name)
{
    if (app_name == NULL) return LV_SYMBOL_FILE;

    for (size_t i = 0; i < sizeof(s_icons) / sizeof(s_icons[0]); i++) {
        if (strcmp(s_icons[i].name, app_name) == 0) return s_icons[i].symbol;
    }
    return LV_SYMBOL_FILE;
}

esp_err_t fw_asset_init(void)
{
    ESP_LOGI(TAG, "initialized (Montserrat + CN 14/16 px + GB2312 fallback + symbol icons)");
    return ESP_OK;
}
