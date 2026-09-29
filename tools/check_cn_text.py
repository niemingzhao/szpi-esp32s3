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
TARGETS = ['apps', 'framework/src', 'framework/include', 'main', 'services/src', 'peripherals/src', 'drivers/src']

CJK = re.compile(r'[\u3000-\u303F\u4E00-\u9FFF\uFF00-\uFFEF]')

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
