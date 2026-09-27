/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * 中文字体声明（实现由 tools/gen_fw_fonts.py 生成到 assets/fw_fonts.c）
 *
 * font_cn14 / font_cn16  GB2312 全集（6763 汉字 + 682 符号），14 / 16 px，4 bpp
 * font_cn_extra          GBK 全集（20902 汉字），14 px，2 bpp，回退用
 *
 * 界面文案直接用这两套界面字体，不需要维护手工字表；GB2312 以外的生僻字由
 * font_cn_extra 兜底，最后回退 lv_font_montserrat_14。
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(font_cn14);
LV_FONT_DECLARE(font_cn16);
LV_FONT_DECLARE(font_cn_extra);

#ifdef __cplusplus
}
#endif
