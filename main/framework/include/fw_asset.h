/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Asset
 *
 * 统一资源入口：字体与 LVGL 文件系统驱动（'A' 盘，供 LVGL 的图片解码按路径读文件）。
 * 中文用 Noto Sans SC（GB2312 全集的 14 / 16 px，回退 GBK 全集），拉丁用 Montserrat；
 * 图标见 fw_icons.h：界面功能图标 icon_ui_*、桌面图标 icon_home_*、状态栏图标 icon_status_*。
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化资源模块（字体已内置；这里注册 LVGL 的 'A' 盘文件系统驱动并打一条日志）
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
 * @brief 把绝对路径转成 LVGL 能读的路径（加内部磁盘号，如 "A:/sdcard/a.png"）
 *
 * LVGL 的图片解码自己读文件，必须走注册过的 lv_fs_drv；用本函数转换后再交给
 * lv_image_set_src()。目录列表 / 写文件仍用 svc_storage 的接口。
 */
esp_err_t fw_asset_fs_path(const char *path, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
