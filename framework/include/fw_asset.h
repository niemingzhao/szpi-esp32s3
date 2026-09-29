/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Asset
 *
 * 统一资源入口：字体与图标。
 * 中文用 Noto Sans SC 子集（14 / 16 px，含 GB2312 回退字体），拉丁用 Montserrat，
 * 图标优先 LVGL 内置符号；App 专用 64x64 图标资源也从这里取。
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化资源模块（字体与符号已内置，不加载外部资源，这里只打一条日志）
 */
esp_err_t fw_asset_init(void);

/**
 * @brief 字体：小号（14 px，拉丁 / 数字），不可用时回退 LVGL 默认字体
 */
const lv_font_t *fw_asset_font_14(void);

/**
 * @brief 字体：中号（20 px），不可用时回退 LVGL 默认字体
 */
const lv_font_t *fw_asset_font_20(void);

/**
 * @brief 字体：常规（24 px）
 */
const lv_font_t *fw_asset_font_24(void);

/**
 * @brief 字体：大号（32 px），未启用时回退 24 px
 */
const lv_font_t *fw_asset_font_32(void);

/**
 * @brief 字体：中文（14 px，正文 / 标签；同样覆盖 ASCII）
 */
const lv_font_t *fw_asset_font_cn(void);

/**
 * @brief 字体：中文大号（16 px，标题 / 强调）
 */
const lv_font_t *fw_asset_font_cn_large(void);

/**
 * @brief 按 App 名取内置符号图标（无匹配时返回通用文件图标）
 */
const char *fw_asset_symbol_for(const char *app_name);

/**
 * @brief 把绝对路径转成 LVGL 能读的路径（加内部磁盘号，如 "A:/sdcard/a.png"）
 *
 * LVGL 的图片解码自己读文件，必须走注册过的 lv_fs_drv；用本函数转换后再交给
 * lv_img_set_src()。目录列表 / 写文件仍用 svc_storage 的接口。
 */
esp_err_t fw_asset_fs_path(const char *path, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
