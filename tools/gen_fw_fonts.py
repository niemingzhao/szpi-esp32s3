#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成中文字体（main/framework/assets/fw_fonts.c 与 include/fw_fonts.h）。

三套字体内联在同一个 C 文件里（文件尽量合并，不要把字体拆散成多个源文件）：

  font_cn14       GB2312 全集（6763 汉字 + 682 符号），14 px / 4 bpp
  font_cn16       GB2312 全集，16 px / 4 bpp
  font_cn_extra   GBK 全集（20902 汉字），14 px / 2 bpp，作 font_cn14/16 的回退

font_cn14 / font_cn16 是先编译进固件的界面字体，覆盖所有简体常用字，界面文案不需要再维护
手工字表；GB2312 以外的生僻字（人名、生僻地名）由 font_cn_extra 兜底，最后才回退到
lv_font_montserrat_14（Latin + LVGL 内置符号）。

字体内全部按 4 倍超采样栅格化后取阈值，字节按 LVGL 9 的 fmt_txt 格式打包（源码格式见
lv_font_fmt_txt.h）。

用法（源字体默认用工程内的 tools/fonts/NotoSansSC-VF.ttf）：
    python tools/gen_fw_fonts.py
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_C = os.path.join(ROOT, "main", "framework", "assets", "fw_fonts.c")
ASSET_H = os.path.join(ROOT, "main", "framework", "include", "fw_fonts.h")

ASCII_RANGE = range(0x20, 0x7F)

# (字体名, 字号, bpp, 字符集, 回退字体)
FONTS = (
    ("font_cn14", 14, 4, "gb2312", "font_cn_extra"),
    ("font_cn16", 16, 4, "gb2312", "font_cn_extra"),
    ("font_cn_extra", 14, 2, "gbk", "lv_font_montserrat_14"),
)


def gb2312_chars():
    """GB2312 全集：符号区（0xA1~0xA9）+ 一级 / 二级汉字（0xB0~0xF7）。"""
    out = []
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return out


def gbk_chars():
    """GBK 全集（含 GB2312）：0x81~0xFE / 0x40~0xFE 范围内能解码的字符。"""
    out = []
    for hi in range(0x81, 0xFF):
        for lo in range(0x40, 0xFF):
            if lo == 0x7F:
                continue
            try:
                out.append(bytes([hi, lo]).decode("gbk"))
            except UnicodeDecodeError:
                continue
    return out


def charset(name):
    extra = gb2312_chars() if name == "gb2312" else gbk_chars()
    return set(chr(c) for c in ASCII_RANGE) | set(extra)


def glyph_of(font, ascent, descent, ch, bpp):
    """返回 (adv_w, box_w, box_h, ofs_x, ofs_y, bytes)"""
    adv_w = int(round(font.getlength(ch) * 16))

    pad = 6
    w = int(font.getlength(ch)) + 2 * pad + 6
    h = ascent + descent + 2 * pad

    img = Image.new("L", (max(w, 1), max(h, 1)), 0)
    ImageDraw.Draw(img).text((pad, pad + ascent), ch, font=font, fill=255, anchor="ls")
    box = img.getbbox()
    if box is None:                      # 空格等无墨迹字形
        return adv_w, 0, 0, 0, 0, b""

    left, top, right, bottom = box
    box_w = right - left
    box_h = bottom - top
    ofs_x = left - pad
    ofs_y = -((bottom) - (pad + ascent))   # 下沿低于基线为正 -> 取负

    levels = (1 << bpp) - 1
    px = list(img.crop(box).tobytes())
    vals = [(v * levels + 127) // 255 for v in px]

    out = bytearray()
    per = 8 // bpp
    for i in range(0, len(vals), per):
        byte = 0
        for k in range(per):
            v = vals[i + k] if i + k < len(vals) else 0
            byte = (byte << bpp) | (v & levels)
        out.append(byte)

    return adv_w, box_w, box_h, ofs_x, ofs_y, bytes(out)


def emit_font(ttf, name, size, bpp, chars, fallback):
    """把一套字体渲染成 C 代码行（静态符号带字体名前缀，三套共存于同一文件）。"""
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent

    glyphs = []
    for ch in sorted(chars):
        adv_w, bw, bh, ox, oy, data = glyph_of(font, ascent, descent, ch, bpp)
        glyphs.append((ord(ch), adv_w, bw, bh, ox, oy, data))

    ascii_glyphs = [g for g in glyphs if g[0] < 128]
    other = [g for g in glyphs if g[0] >= 128]
    ordered = ascii_glyphs + other

    bitmap = bytearray()
    dsc = ['    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},'
           '   /* id = 0 reserved */']
    for cp, adv_w, bw, bh, ox, oy, data in ordered:
        dsc.append('    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},'
                   % (len(bitmap), adv_w, bw, bh, ox, oy))
        bitmap += data

    gid_start = 1 + len(ascii_glyphs)
    rng_start = other[0][0]
    # 注意：LVGL 的 cmap 判定是 rcp < range_length，所以 range_length 必须含端点
    # （写成 last - first 会让码点最大的那个字永远查不到字形，LV_USE_FONT_PLACEHOLDER
    #  开启时显示成方框）。
    rng_len = other[-1][0] - rng_start + 1
    ulist = [cp - rng_start for cp, *_ in other]
    if max(ulist) > 0xFFFF or rng_len > 0xFFFF:
        raise SystemExit('%s 的编码跨度超过 16 位，需要拆分 cmap' % name)

    L = []
    L.append('/* ------------------------------ %s ------------------------------ */' % name)
    L.append('')
    L.append('static LV_ATTRIBUTE_LARGE_CONST const uint8_t %s_bitmap[] = {' % name)
    for i in range(0, len(bitmap), 16):
        L.append('    ' + ', '.join('0x%02x' % b for b in bitmap[i:i + 16]) + ',')
    L.append('};')
    L.append('')
    L.append('static const lv_font_fmt_txt_glyph_dsc_t %s_glyph_dsc[] = {' % name)
    L.extend(dsc)
    L.append('};')
    L.append('')
    L.append('static const uint16_t %s_unicode_list[] = {' % name)
    for i in range(0, len(ulist), 12):
        L.append('    ' + ', '.join('0x%x' % v for v in ulist[i:i + 12]) + ',')
    L.append('};')
    L.append('')
    L.append('static const lv_font_fmt_txt_cmap_t %s_cmaps[] = {' % name)
    L.append('    {   /* ASCII */')
    L.append('        .range_start = 32, .range_length = 95, .glyph_id_start = 1,')
    L.append('        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,')
    L.append('        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY')
    L.append('    },')
    L.append('    {   /* 汉字 / 全角符号 */')
    L.append('        .range_start = %d, .range_length = %d, .glyph_id_start = %d,'
             % (rng_start, rng_len, gid_start))
    L.append('        .unicode_list = %s_unicode_list, .glyph_id_ofs_list = NULL, .list_length = %d,'
             % (name, len(ulist)))
    L.append('        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY')
    L.append('    }')
    L.append('};')
    L.append('')
    L.append('static const lv_font_fmt_txt_dsc_t %s_dsc = {' % name)
    L.append('    .glyph_bitmap = %s_bitmap,' % name)
    L.append('    .glyph_dsc = %s_glyph_dsc,' % name)
    L.append('    .cmaps = %s_cmaps,' % name)
    L.append('    .kern_dsc = NULL,')
    L.append('    .kern_scale = 0,')
    L.append('    .cmap_num = 2,')
    L.append('    .bpp = %d,' % bpp)
    L.append('    .kern_classes = 0,')
    L.append('    .bitmap_format = 0')
    L.append('};')
    L.append('')
    L.append('const lv_font_t %s = {' % name)
    L.append('    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,')
    L.append('    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,')
    L.append('    .line_height = %d,' % line_height)
    L.append('    .base_line = %d,' % descent)
    L.append('    .subpx = LV_FONT_SUBPX_NONE,')
    L.append('    .underline_position = -1,')
    L.append('    .underline_thickness = 1,')
    L.append('    .dsc = &%s_dsc,' % name)
    L.append('    .fallback = &%s' % fallback)
    L.append('};')
    L.append('')

    print('%-16s size=%2d bpp=%d  glyphs=%6d  bitmap=%7d B'
          % (name, size, bpp, len(ordered), len(bitmap)))
    return L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ttf', nargs='?', default=None,
                    help='源 TTF/OTF；省略时用工程内 tools/fonts/NotoSansSC-VF.ttf')
    args = ap.parse_args()

    ttf = args.ttf or os.path.join(ROOT, 'tools', 'fonts', 'NotoSansSC-VF.ttf')
    if not os.path.isfile(ttf):
        raise SystemExit('找不到字体源文件：%s\n请把 NotoSansSC-VF.ttf（OFL 授权）放到 tools/fonts/，'
                         '或显式传入路径。' % ttf)

    cache = {}
    lines = [
        '/*******************************************************************************',
        ' * 由 %s 栅格化生成，工具：tools/gen_fw_fonts.py（不要手改）' % os.path.basename(ttf),
        ' *',
        ' * font_cn14 / font_cn16  GB2312 全集（6763 汉字 + 682 符号），4 bpp',
        ' * font_cn_extra          GBK 全集（20902 汉字），2 bpp，作回退',
        ' ******************************************************************************/',
        '',
        '#include "lvgl.h"',
        '#include "fw_fonts.h"',
        '',
    ]

    for name, size, bpp, cs, fallback in FONTS:
        if cs not in cache:
            cache[cs] = charset(cs)
        lines += emit_font(ttf, name, size, bpp, cache[cs], fallback)

    os.makedirs(os.path.dirname(ASSET_C), exist_ok=True)
    with open(ASSET_C, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))

    header = [
        '/*',
        ' * SPDX-FileCopyrightText: 2026 SZPI-OS',
        ' *',
        ' * 中文字体声明（实现由 tools/gen_fw_fonts.py 生成到 assets/fw_fonts.c）',
        ' *',
        ' * font_cn14 / font_cn16  GB2312 全集（6763 汉字 + 682 符号），14 / 16 px，4 bpp',
        ' * font_cn_extra          GBK 全集（20902 汉字），14 px，2 bpp，回退用',
        ' *',
        ' * 界面文案直接用这两套界面字体，不需要维护手工字表；GB2312 以外的生僻字由',
        ' * font_cn_extra 兜底，最后回退 lv_font_montserrat_14。',
        ' */',
        '',
        '#pragma once',
        '',
        '#include "lvgl.h"',
        '',
        '#ifdef __cplusplus',
        'extern "C" {',
        '#endif',
        '',
        'LV_FONT_DECLARE(font_cn14);',
        'LV_FONT_DECLARE(font_cn16);',
        'LV_FONT_DECLARE(font_cn_extra);',
        '',
        '#ifdef __cplusplus',
        '}',
        '#endif',
        '',
    ]
    os.makedirs(os.path.dirname(ASSET_H), exist_ok=True)
    with open(ASSET_H, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(header))

    print('-> %s' % os.path.relpath(ASSET_C, ROOT).replace(os.sep, '/'))
    print('-> %s' % os.path.relpath(ASSET_H, ROOT).replace(os.sep, '/'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
