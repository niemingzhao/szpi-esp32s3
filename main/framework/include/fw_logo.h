/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * 开机 Logo 声明（实现由 assets/fw_logo.c 提供）
 *
 * 立创·实战派官方资源，120x120，格式 LV_COLOR_FORMAT_RGB565A8。RGB565 平面按小端
 * 存放：LVGL 的图片数据必须与显示缓冲同字节序，写屏回调 periph_lcd_flush_cb() 会再
 * 交换一次送给 ST7789。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_IMG_DECLARE(image_lckfb_logo);

#ifdef __cplusplus
}
#endif
