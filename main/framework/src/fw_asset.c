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
#include <stdbool.h>
#include <stdio.h>
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

/* ------------------------- LVGL 文件系统驱动（'A' 盘） -------------------------
 *
 * LVGL 的图片解码（PNG / GIF / BMP）需要自己按路径读文件，而 LVGL 读文件必须有一个
 * 已注册的 lv_fs_drv。驱动只能注册到 LVGL（Framework 层持有），所以放在 fw_asset 里；
 * 路径最终由 POSIX 文件 API 走 VFS（/sdcard 或 /internal）。
 *
 * App 侧统一用 fw_asset_fs_path() 把绝对路径转成 LVGL 路径（"A:/sdcard/a.png"）。
 */

#define FW_ASSET_FS_LETTER   'A'

static lv_fs_drv_t s_fs_drv;
static bool s_fs_registered = false;

/* LVGL 传进来的路径可能带盘符，统一去掉 "A:" 前缀 */
static const char *fs_vfs_path(const char *path)
{
    if (path != NULL && path[0] != '\0' && path[1] == ':') {
        return path + 2;
    }
    return path;
}

static void *fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    if (mode != LV_FS_MODE_RD) return NULL;   /* 只读：写文件走 svc_storage */

    return (void *)fopen(fs_vfs_path(path), "rb");
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    if (file_p == NULL) return LV_FS_RES_INV_PARAM;

    fclose((FILE *)file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    if (file_p == NULL || buf == NULL || br == NULL) return LV_FS_RES_INV_PARAM;

    size_t n = fread(buf, 1, btr, (FILE *)file_p);
    *br = (uint32_t)n;
    return (n == btr) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    if (file_p == NULL) return LV_FS_RES_INV_PARAM;

    int w = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
    else if (whence == LV_FS_SEEK_END) w = SEEK_END;

    return (fseek((FILE *)file_p, (long)pos, w) == 0) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    if (file_p == NULL || pos_p == NULL) return LV_FS_RES_INV_PARAM;

    long pos = ftell((FILE *)file_p);
    if (pos < 0) return LV_FS_RES_FS_ERR;

    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

esp_err_t fw_asset_fs_path(const char *path, char *buf, size_t len)
{
    if (path == NULL || buf == NULL || len < 4) return ESP_ERR_INVALID_ARG;

    int n = snprintf(buf, len, "%c:%s", FW_ASSET_FS_LETTER, path);
    return (n > 0 && (size_t)n < len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t fw_asset_init(void)
{
    if (!s_fs_registered) {
        lv_fs_drv_init(&s_fs_drv);
        s_fs_drv.letter = FW_ASSET_FS_LETTER;
        s_fs_drv.open_cb = fs_open_cb;
        s_fs_drv.close_cb = fs_close_cb;
        s_fs_drv.read_cb = fs_read_cb;
        s_fs_drv.seek_cb = fs_seek_cb;
        s_fs_drv.tell_cb = fs_tell_cb;
        lv_fs_drv_register(&s_fs_drv);
        s_fs_registered = true;
    }

    ESP_LOGI(TAG, "initialized (Montserrat + CN 14/16 px + GB2312 fallback + symbol icons, fs '%c')",
             FW_ASSET_FS_LETTER);
    return ESP_OK;
}
