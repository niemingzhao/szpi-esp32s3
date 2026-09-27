# -*- coding: utf-8 -*-
"""检查界面源码里用到的非 ASCII 字符是否都在中文字体覆盖范围内（防止出现方框）。

字体覆盖范围见 tools/gen_fw_fonts.py：font_cn14 / font_cn16 是 GB2312 全集，
font_cn_extra 是 GBK 全集（回退）。所以这里按"ASCII ∪ GBK"判定，不需要手工字表；
同时检查生成出来的字体 cmap 是否"含端点"（LVGL 判定 rcp < range_length）。

用法：
    python tools/check_cn_text.py
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGETS = ['main']
FONT_SRC = 'main/framework/assets/fw_fonts.c'


def is_covered(ch):
    """字体链能不能渲染这个字符：ASCII 直接可以，其余看 GBK（font_cn_extra 的范围）。"""
    if ord(ch) < 0x80:
        return True
    try:
        ch.encode('gbk')
        return True
    except UnicodeEncodeError:
        return False


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
        if os.path.basename(dirpath) == 'assets':      # 生成的资源文件不参与文案检查
            continue
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            p = Path(dirpath) / fn
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                text = f.read()
            # 先去掉注释：注释里的引号（如 "正在处理事件"）会被误当成字符串字面量
            text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
            text = re.sub(r'(?m)(^|\s)//[^\n]*', ' ', text)
            # 只看字符串字面量里的非 ASCII 字符
            for lit in re.findall(r'"([^"\n]*)"', text):
                for ch in lit:
                    if not is_covered(ch):
                        missing.setdefault(ch, set()).add(str(p.relative_to(ROOT)))

# 内置示例脚本：构建期用 EMBED_FILES 进固件，脚本里的中文最终由 LVGL 渲染，也要能显示。
# 它们不是 .c/.h、又在 assets/ 下，上面的遍历会跳过，所以单独扫一遍（注释里的中文一起算，
# 生僻字同样值得提醒）。
SCRIPT_ASSETS = ROOT / 'main/framework/assets/scripts'
if SCRIPT_ASSETS.is_dir():
    for p in sorted(SCRIPT_ASSETS.iterdir()):
        if p.suffix not in ('.lua', '.txt'):
            continue
        text = p.read_text(encoding='utf-8', errors='replace')
        for ch in text:
            if not is_covered(ch):
                missing.setdefault(ch, set()).add(str(p.relative_to(ROOT)))

if not missing:
    print('OK: 字符串里的非 ASCII 字符都能被字体覆盖（ASCII ∪ GBK）')
else:
    print('有 %d 个字符字体覆盖不到（会显示成方框）：' % len(missing))
    for ch, files in sorted(missing.items()):
        print('  %s  U+%04X   %s' % (ch, ord(ch), ', '.join(sorted(files)[:3])))
    print()
    print('若是界面文案用字，考虑改用别的字；确实需要就扩大字体覆盖范围后重新生成：')
    print('    python tools/gen_fw_fonts.py')
    sys.exit(1)

# 字体 cmap 必须"含端点"，否则码点最大的字会变成方框
ok, msg = check_font_cmap(ROOT / FONT_SRC)
if not ok:
    print(msg)
    print('字体 cmap 的 range_length 没覆盖最后一个码点（LVGL 判定 rcp < range_length），'
          '码点最大的字会显示成方框。')
    print('重新生成字体：python tools/gen_fw_fonts.py')
    sys.exit(1)

print('OK: 字体 cmap 覆盖完整（含码点最大的字）')
