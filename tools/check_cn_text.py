# -*- coding: utf-8 -*-
"""检查界面源码里用到的汉字是否都在中文字体子集字表内（防止出现方框）。

字表在 tools/cn_chars.py，与本脚本共用；本脚本不依赖 Pillow。
用法：
    python tools/check_cn_text.py
"""
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cn_chars import CN_CHARS

covered = set(CN_CHARS) | set(chr(c) for c in range(0x20, 0x7F))

ROOT = Path(__file__).resolve().parent.parent
TARGETS = ['main']

CJK = re.compile(r'[\u3000-\u303F\u4E00-\u9FFF\uFF00-\uFFEF]')


def check_font_cmap(path):
    """检查生成后的字体 cmap 是否"含端点"。

    LVGL 查表用的是 rcp < range_length（见 lv_font_fmt_txt.c），所以 range_length 必须
    覆盖到最后一个码点。生成脚本若写成 last - first（少 1），码点最大的那个字就查不到
    字形，配合 LV_USE_FONT_PLACEHOLDER=y 会显示成方框。
    """
    text = path.read_text(encoding='utf-8', errors='replace')
    for m in re.finditer(r'\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*\n'
                         r'\s*\.unicode_list = (\w+),', text):
        ulist = m.group(4)
        if ulist == 'NULL':
            continue
        body = re.search(r'static const uint16_t %s\[\] = \{(.*?)\};' % ulist, text, re.S)
        if body is None:
            continue
        offs = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body.group(1))]
        if offs and max(offs) >= int(m.group(2)):
            return False, '  %s：range_length=%s 没覆盖最后一个码点（max offset=%d）' % (
                path.relative_to(ROOT), m.group(2), max(offs))
    return True, ''

missing = {}
for d in TARGETS:
    base = ROOT / d
    for dirpath, _, files in os.walk(base):
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            p = Path(dirpath) / fn
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                text = f.read()
            # 先去掉注释：注释里的引号（如 "正在处理事件"）会被误当成字符串字面量
            text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
            text = re.sub(r'(?m)(^|\s)//[^\n]*', ' ', text)
            # 只看字符串字面量里的汉字（避免把注释算进来造成误报太多）
            for lit in re.findall(r'"([^"\n]*)"', text):
                for ch in CJK.findall(lit):
                    if ch not in covered:
                        missing.setdefault(ch, set()).add(str(p.relative_to(ROOT)))

if not missing:
    print('OK: 字符串里的汉字全部在字表内')
else:
    print('缺少 %d 个字（在字符串里用到但字表没有）：' % len(missing))
    for ch, files in sorted(missing.items()):
        print('  %s  U+%04X   %s' % (ch, ord(ch), ', '.join(sorted(files)[:3])))
    print()
    print('把这些字加进 tools/cn_chars.py 的 CN_CHARS 后重新生成字体即可：')
    print(''.join(sorted(missing.keys())))
    sys.exit(1)

# 字体 cmap 必须"含端点"，否则码点最大的字会变成方框
bad_fonts = []
for rel in ('main/framework/assets/font_cn14.c', 'main/framework/assets/font_cn16.c'):
    ok, msg = check_font_cmap(ROOT / rel)
    if not ok:
        bad_fonts.append(msg)

if bad_fonts:
    for m in bad_fonts:
        print(m)
    print('字体 cmap 的 range_length 没覆盖最后一个码点（LVGL 判定 rcp < range_length），'
          '码点最大的字会显示成方框。')
    print('重新生成字体：python tools/gen_cn_font.py --sizes 14,16')
    sys.exit(1)

print('OK: 字体 cmap 覆盖完整（含码点最大的字）')
