#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：代码里不要直接调 printf / fprintf / puts（统一走 ESP_LOGx）。

snprintf / vsnprintf / vprintf 这些不算：前者是格式化到缓冲，后者是日志钩子里
转发给原输出的兜底路径，都是必要的。

用法：
    python tools/check_printf.py
退出码：0 = 通过，1 = 有直接输出（列出文件与位置）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SKIP_DIRS = ('managed_components', 'build', '.git', 'assets')

CALL = re.compile(r'(?<![A-Za-z0-9_])(printf|fprintf|puts|fputs)\s*\(')


def main():
    findings = []
    for root, dirs, files in os.walk(os.path.join(ROOT, 'main')):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in sorted(files):
            if not name.endswith('.c'):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                for lineno, line in enumerate(f, 1):
                    s = line.strip()
                    if s.startswith(('*', '/*', '//')):
                        continue
                    m = CALL.search(line)
                    if m:
                        findings.append((rel, lineno, m.group(1), s[:100]))

    if not findings:
        print('OK: 没有直接使用 printf / fprintf / puts（统一走 ESP_LOGx）')
        return 0

    print('发现 %d 处直接输出（应改用 ESP_LOGx）：' % len(findings))
    for path, lineno, what, text in findings:
        print('  %s:%d  %s' % (path, lineno, what))
        print('      %s' % text)
    return 1


if __name__ == '__main__':
    sys.exit(main())
