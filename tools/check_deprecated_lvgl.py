#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：不要用 LVGL 已废弃的旧 API 名。

LVGL 9 把一批旧名字保留成别名，集中放在 include/lvgl/api_map/ 下，两种形式：
    #define LV_LABEL_LONG_DOT LV_LABEL_LONG_MODE_DOTS
    typedef lv_screen_load_anim_t lv_scr_load_anim_t;
用旧名虽然还能编译，但后续版本会删掉。本脚本读 api_map 收集"旧名 -> 新名"，再在项目
代码里找旧名（忽略注释）。

一个例外：别名里有些名字（如 lv_coord_t）本身也是 LVGL 9 的正常符号，只是正好被 api_map
再 typedef 了一次；这类名字在非 api_map 的头文件里能独立找到，按"不是别名"处理。

用法：
    python tools/check_deprecated_lvgl.py
退出码：0 = 通过，1 = 用到了废弃别名（列出文件与位置）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
LVGL = os.path.join(ROOT, 'managed_components', 'lvgl__lvgl')
API_MAP = os.path.join(LVGL, 'include', 'lvgl', 'api_map')
SKIP_DIRS = ('managed_components', 'build', '.git', 'assets')

IDENT = re.compile(r'[A-Za-z_]\w*')
DEF = re.compile(r'^\s*#\s*define\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*$')
TYPEDEF = re.compile(r'^\s*typedef\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;\s*$')


def real_identifiers():
    """api_map 之外的头文件里出现过的标识符：这些名字是 LVGL 的正常符号，不算废弃。"""
    ids = set()
    for root, dirs, files in os.walk(LVGL):
        if os.path.normpath(root).startswith(os.path.normpath(API_MAP)):
            continue
        for name in files:
            if not name.endswith('.h'):
                continue
            with open(os.path.join(root, name), 'r', encoding='utf-8', errors='ignore') as f:
                ids |= set(IDENT.findall(f.read()))
    return ids


def collect_aliases(real):
    aliases = {}
    if not os.path.isdir(API_MAP):
        return aliases
    for name in sorted(os.listdir(API_MAP)):
        if not name.endswith('.h'):
            continue
        with open(os.path.join(API_MAP, name), 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                m = DEF.match(line)
                if m:
                    old, new = m.group(1), m.group(2)
                else:
                    m = TYPEDEF.match(line)
                    if not m:
                        continue
                    new, old = m.group(1), m.group(2)   # typedef <规范名> <旧名>;
                if old == new or old.startswith('_') or old in real:
                    continue
                aliases.setdefault(old, new)
    return aliases


def main():
    aliases = collect_aliases(real_identifiers())
    if not aliases:
        print('找不到 LVGL 的 api_map（%s），跳过' % API_MAP)
        return 0

    findings = []
    for root, dirs, files in os.walk(os.path.join(ROOT, 'main')):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in sorted(files):
            if not name.endswith(('.c', '.h')):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                for lineno, line in enumerate(f, 1):
                    s = line.strip()
                    if s.startswith(('*', '/*', '//')):
                        continue
                    for old, new in aliases.items():
                        if re.search(r'(?<![A-Za-z0-9_])' + re.escape(old) + r'(?![A-Za-z0-9_])', line):
                            findings.append((rel, lineno, old, new))
                            break

    if not findings:
        print('OK: 没有用到 LVGL 已废弃的 API 别名')
        return 0

    print('发现 %d 处 LVGL 废弃别名（改用右侧的新名）：' % len(findings))
    for path, lineno, old, new in findings:
        print('  %s:%d  %s -> %s' % (path, lineno, old, new))
    return 1


if __name__ == '__main__':
    sys.exit(main())
