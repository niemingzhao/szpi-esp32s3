#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用 Pillow 从 TTF 生成 LVGL 字体 C 文件。

两类字体：
  1) UI 字体（--cs ui，默认）：只含界面用字（CN_CHARS + ASCII），14/16 px 两档，
     回退到 font_cn_extra（覆盖任意常见汉字，比如 Wi-Fi 名称）。
  2) 回退字体（--cs gb2312）：GB2312 一级字库（3755 个常用汉字）+ ASCII，
     默认 14 px / 2bpp，供 UI 字体在遇到字表外汉字时回退使用。

用法：
  python tools/gen_cn_font.py C:\\Windows\\Fonts\\NotoSansSC-VF.ttf --sizes 14,16
  python tools/gen_cn_font.py C:\\Windows\\Fonts\\NotoSansSC-VF.ttf --cs gb2312 --sizes 14 --bpp 2

界面新增中文文案若出现方框，把该字加进 CN_CHARS 后重新生成 UI 字体。
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# 界面用字表（新增中文文案时把新字加进来）
CN_CHARS = (
    "控制中心亮度音量试听通知清空暂无电源请选择操作关机重启取消确定"
    "音频不可用时间未同步清除已保存的凭据设置关于返回主屏关闭"
    "网络系统无线局域密码扫描连接失败输入成功错误信号强度地址自动获取静态名称隐藏"
    "时钟音乐文件相机录音计算器编辑器图片视频浏览器更新调试工厂"
    "播放暂停上一首下一首列表循环随机静音专辑歌手歌曲时长进度"
    "中英文简体繁体语言版本背光超时主题深浅色默认恢复出厂重置存储空间可用已用总大小删除是否"
    "秒分时年月日星期一星温度电量正在加载完成重试"
    "开注销登陆账号用户名"
    "摄像头拍照相册"
    "蓝牙手电筒锁屏"
    "二三四五六周断"
    "余全内到剩发将常忘效显核次点热现生示芯记部页"
    "选择输入确定取消关机重启信号强板卡闲占主连接成功失败"
    "秒钟分时年月日星期上下左右前后大小多少开关启停"
    "，。：、？！（）—…·“”‘’《》"
    "切换没有"    # 主题已切换 / 没有已保存的网络
)

ASCII_RANGE = range(0x20, 0x7F)


def gb2312_level1():
    """GB2312 一级字库（按拼音排序的 3755 个常用汉字）。"""
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode('gb2312'))
            except UnicodeDecodeError:
                continue
    return out


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


def emit(ttf, size, out_path, name, chars, bpp, fallback):
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
    dsc = ['    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,']
    for cp, adv_w, bw, bh, ox, oy, data in ordered:
        dsc.append('    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},'
                   % (len(bitmap), adv_w, bw, bh, ox, oy))
        bitmap += data

    gid_start = 1 + len(ascii_glyphs)
    rng_start = other[0][0]
    rng_len = other[-1][0] - rng_start
    ulist = [cp - rng_start for cp, *_ in other]
    if max(ulist) > 0xFFFF or rng_len > 0xFFFF:
        raise SystemExit('编码跨度超过 16 位，需要拆分 cmap')

    L = []
    L.append('/*******************************************************************************')
    L.append(' * 由 %s 栅格化生成（%d px / %d bpp），工具：tools/gen_cn_font.py' % (os.path.basename(ttf), size, bpp))
    L.append(' * Glyphs: %d   Bitmap: %d bytes' % (len(ordered), len(bitmap)))
    L.append(' ******************************************************************************/')
    L.append('')
    L.append('#include "lvgl.h"')
    if fallback.startswith('font_'):
        L.append('')
        L.append('LV_FONT_DECLARE(%s);' % fallback)
    L.append('')
    L.append('static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {')
    for i in range(0, len(bitmap), 16):
        L.append('    ' + ', '.join('0x%02x' % b for b in bitmap[i:i + 16]) + ',')
    L.append('};')
    L.append('')
    L.append('static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {')
    L.extend(dsc)
    L.append('};')
    L.append('')
    L.append('static const uint16_t unicode_list_1[] = {')
    for i in range(0, len(ulist), 12):
        L.append('    ' + ', '.join('0x%x' % v for v in ulist[i:i + 12]) + ',')
    L.append('};')
    L.append('')
    L.append('static const lv_font_fmt_txt_cmap_t cmaps[] = {')
    L.append('    {   /* ASCII */')
    L.append('        .range_start = 32, .range_length = 95, .glyph_id_start = 1,')
    L.append('        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,')
    L.append('        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY')
    L.append('    },')
    L.append('    {   /* 汉字 / 全角标点 */')
    L.append('        .range_start = %d, .range_length = %d, .glyph_id_start = %d,'
             % (rng_start, rng_len, gid_start))
    L.append('        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = %d,'
             % len(ulist))
    L.append('        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY')
    L.append('    }')
    L.append('};')
    L.append('')
    L.append('static lv_font_fmt_txt_glyph_cache_t cache;')
    L.append('static const lv_font_fmt_txt_dsc_t font_dsc = {')
    L.append('    .glyph_bitmap = glyph_bitmap,')
    L.append('    .glyph_dsc = glyph_dsc,')
    L.append('    .cmaps = cmaps,')
    L.append('    .kern_dsc = NULL,')
    L.append('    .kern_scale = 0,')
    L.append('    .cmap_num = 2,')
    L.append('    .bpp = %d,' % bpp)
    L.append('    .kern_classes = 0,')
    L.append('    .bitmap_format = 0,')
    L.append('    .cache = &cache')
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
    L.append('    .dsc = &font_dsc,')
    L.append('    .fallback = &%s' % fallback)
    L.append('};')
    L.append('')

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L))

    print('%-22s size=%2d bpp=%d  glyphs=%d  bitmap=%d B'
          % (os.path.basename(out_path), size, bpp, len(ordered), len(bitmap)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ttf')
    ap.add_argument('--sizes', default='14,16')
    ap.add_argument('--cs', default='ui', choices=['ui', 'gb2312'])
    ap.add_argument('--bpp', type=int, default=0)
    ap.add_argument('--outdir', default=None)
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outdir = args.outdir or os.path.join(here, 'framework', 'assets')

    if args.cs == 'gb2312':
        size = int(args.sizes.split(',')[0])
        bpp = args.bpp or 2
        chars = set(chr(c) for c in ASCII_RANGE) | set(gb2312_level1())
        emit(args.ttf, size, os.path.join(outdir, 'font_cn_extra.c'), 'font_cn_extra',
             chars, bpp, 'lv_font_montserrat_14')
        return 0

    bpp = args.bpp or 4
    chars = set(chr(c) for c in ASCII_RANGE) | set(CN_CHARS)
    for s in args.sizes.split(','):
        size = int(s)
        emit(args.ttf, size, os.path.join(outdir, 'font_cn%d.c' % size), 'font_cn%d' % size,
             chars, bpp, 'font_cn_extra')
    return 0


if __name__ == '__main__':
    sys.exit(main())
