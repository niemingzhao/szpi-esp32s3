/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Asset 实现
 *
 * 中文用 Noto Sans SC（GB2312 全集的 14 / 16 px，回退 GBK 全集），拉丁用 Montserrat；
 * 末尾是 LVGL 的 'A' 盘文件系统驱动，把路径交给 POSIX 文件 API（走 VFS）。
 */

#include "fw_common.h"
#include "fw_fonts.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "fw.asset";

/* ------------------------- 拉丁字体带中文回退 -------------------------
 *
 * LVGL 内置的 Montserrat 只有基本拉丁加少量符号（°、• 等），× ÷ 这类字符查不到字形就
 * 显示成方框（计算器的 × ÷ 键踩过）。这里给用到的几号拉丁字体各做一份运行时副本，把
 * fallback 指到中文字体上：Montserrat 里有的字形照旧走它（字号 / 行高不变），没有的
 * （× ÷ 等）回退到中文字体去取。回退字体按最接近的字号挑（14 用 cn14，20 及以上用 cn16）。
 *
 * 注意别指成环：中文链是 font_cn14 / font_cn16 → font_cn_extra → lv_font_montserrat_14，
 * 指 font_cn14 / font_cn16 都不会绕回带 fallback 的副本，整条链是有向无环的。 */

static lv_font_t s_latin14;
static lv_font_t s_latin20;
static lv_font_t s_latin24;
static lv_font_t s_latin32;
static bool s_latin14_ready = false;
static bool s_latin20_ready = false;
static bool s_latin24_ready = false;
static bool s_latin32_ready = false;

const lv_font_t *fw_asset_font_14(void)
{
#if LV_FONT_MONTSERRAT_14
    if (!s_latin14_ready) {
        s_latin14 = lv_font_montserrat_14;
        s_latin14.fallback = &font_cn14;
        s_latin14_ready = true;
    }
    return &s_latin14;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *fw_asset_font_20(void)
{
#if LV_FONT_MONTSERRAT_20
    if (!s_latin20_ready) {
        s_latin20 = lv_font_montserrat_20;
        s_latin20.fallback = &font_cn16;
        s_latin20_ready = true;
    }
    return &s_latin20;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *fw_asset_font_24(void)
{
#if LV_FONT_MONTSERRAT_24
    if (!s_latin24_ready) {
        s_latin24 = lv_font_montserrat_24;
        s_latin24.fallback = &font_cn16;
        s_latin24_ready = true;
    }
    return &s_latin24;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *fw_asset_font_32(void)
{
#if LV_FONT_MONTSERRAT_32
    if (!s_latin32_ready) {
        s_latin32 = lv_font_montserrat_32;
        s_latin32.fallback = &font_cn16;
        s_latin32_ready = true;
    }
    return &s_latin32;
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

    /* 读到文件尾时 fread 会少读几个字节，这不算错误：把实际读到的长度报上去即可
     * （LVGL 自带的 posix / stdio 驱动也是这么做的，否则图片解码在文件尾会报错） */
    *br = (uint32_t)fread(buf, 1, btr, (FILE *)file_p);
    return LV_FS_RES_OK;
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
        if (lvgl_port_lock(0)) {
            lv_fs_drv_init(&s_fs_drv);
            s_fs_drv.letter = FW_ASSET_FS_LETTER;
            s_fs_drv.open_cb = fs_open_cb;
            s_fs_drv.close_cb = fs_close_cb;
            s_fs_drv.read_cb = fs_read_cb;
            s_fs_drv.seek_cb = fs_seek_cb;
            s_fs_drv.tell_cb = fs_tell_cb;
            lv_fs_drv_register(&s_fs_drv);
            lvgl_port_unlock();

            s_fs_registered = true;
        } else {
            ESP_LOGE(TAG, "fs driver register failed: lvgl lock unavailable");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "initialized (Montserrat + GB2312 CN 14/16 px + GBK fallback + symbol icons, fs '%c')",
             FW_ASSET_FS_LETTER);
    /* 打印 LVGL 图片缓存大小（0 = 关），排查图库缩放问题时顺手看一眼 */
    ESP_LOGI(TAG, "image cache: %d byte(s)", (int)LV_CACHE_DEF_SIZE);
    return ESP_OK;
}
