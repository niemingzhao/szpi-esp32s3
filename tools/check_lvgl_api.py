#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：App 层用到的 LVGL 标识符是否存在，以及 snprintf 的截断风险。

背景（都是实际踩过的编译错误，GCC 14 下 -Werror 直接失败）：

1. LVGL 版本之间 API 名字会变，例如 8.3 没有 `LV_LABEL_LONG_SCROLL_RIGHT`
   （只有 WRAP / DOT / SCROLL / SCROLL_CIRCULAR / CLIP），写错就是 undeclared。
2. `-Werror=format-truncation`：把 `entry->name`（svc_storage 的目录项，256 字节）这类
   **长度已知的数组**塞进小缓冲，GCC 能算出可能被截断就直接报错。修法是加精度限定
   （`%.60s`）或把缓冲开够。

用法：
    python tools/check_lvgl_api.py
退出码：0 = 通过；1 = 有 LVGL 标识符找不到（必须修）；截断风险只列出来供人工确认。
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
LVGL_DIR = os.path.join(ROOT, 'managed_components', 'lvgl__lvgl')

LV_IDENT = re.compile(r'\b(lv_[a-z0-9_]+|LV_[A-Z0-9_]+)\b')
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def collect_lvgl_identifiers():
    known = {}
    for base, _dirs, files in os.walk(LVGL_DIR):
        for name in files:
            if not name.endswith('.h'):
                continue
            path = os.path.join(base, name)
            try:
                with open(path, 'r', encoding='utf-8', errors='ignore') as fh:
                    text = fh.read()
            except OSError:
                continue
            for m in LV_IDENT.finditer(text):
                known.setdefault(m.group(1), name)
    return known


def iter_sources():
    for root, dirs, files in os.walk(os.path.join(ROOT, 'main')):
        dirs[:] = [d for d in dirs if d not in ('build', '.git')]
        for name in sorted(files):
            if name.endswith('.c'):
                yield os.path.join(root, name)


def collect_project_defines():
    """项目自己 #define 的 LV_* / lv_* 名字（例如图片资源里的 LV_ATTRIBUTE_IMG_xxx）也算存在"""
    defined = set()
    for layer in ('main', 'managed_components'):
        base = os.path.join(ROOT, layer)
        if not os.path.isdir(base):
            continue
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in ('build', '.git')]
            for name in files:
                if not name.endswith(('.c', '.h')):
                    continue
                try:
                    with open(os.path.join(root, name), 'r', encoding='utf-8', errors='ignore') as fh:
                        for line in fh:
                            m = re.match(r'\s*#\s*define\s+(lv_[a-z0-9_]+|LV_[A-Z0-9_]+)\b', line)
                            if m:
                                defined.add(m.group(1))
                except OSError:
                    continue
    return defined


def main():
    known = collect_lvgl_identifiers()
    if not known:
        print('找不到 LVGL 头文件（%s），跳过' % LVGL_DIR)
        return 0
    for name in collect_project_defines():
        known.setdefault(name, '<project>')

    missing = {}
    truncation = []

    for path in iter_sources():
        rel = os.path.relpath(path, ROOT).replace('\\', '/')
        with open(path, 'r', encoding='utf-8', errors='ignore') as fh:
            for lineno, line in enumerate(fh, 1):
                stripped = line.strip()
                is_comment = stripped.startswith(('*', '/*', '//', '*/'))
                if not is_comment:
                    for m in LV_IDENT.finditer(line):
                        name = m.group(1)
                        if name not in known:
                            missing.setdefault(name, set()).add(rel)
                if 'snprintf(' in line and '%s' in line and not is_comment:
                    truncation.append((rel, lineno, stripped[:110]))

    status = 0
    if missing:
        status = 1
        print('发现 %d 个找不到的 LVGL 标识符（编译会报 undeclared）：' % len(missing))
        for name, files in sorted(missing.items()):
            print('  %-36s %s' % (name, ', '.join(sorted(files))))
    else:
        print('OK: 用到的 LVGL 标识符都能在 lvgl__lvgl 头文件里找到')

    if truncation:
        print()
        print('下面这些 snprintf 带 %%s，请确认目标缓冲够大（GCC 14 的 -Werror=format-truncation 会拦）：')
        for rel, lineno, text in truncation:
            print('  %s:%d: %s' % (rel, lineno, text))

    return status


if __name__ == '__main__':
    sys.exit(main())
