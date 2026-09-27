#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：App 描述符的回调搭配。

  - 必须有 .on_create 以及 .name / .title。
  - 有 .on_pause 的必须同时有 .on_start 或 .on_resume。
    进前台有两条路径：首次创建后走 on_start，从返回栈回来走 on_resume。在 on_pause
    里停掉刷新定时器（或退订事件）的 App，只挂一个恢复点会漏（曾经表现为"从桌面再点
    进来，时钟不再跳"）。

用法：
    python tools/check_app_callbacks.py
退出码：0 = 通过，1 = 有问题（列出文件与描述符）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
APPS = os.path.join(ROOT, 'main', 'apps')

DESC = re.compile(r'const\s+fw_app_desc_t\s+(\w+)\s*=\s*\{(.*?)\n\};', re.S)

REQUIRED = ('.on_create', '.name', '.title')


def main():
    findings = []
    for root, dirs, files in os.walk(APPS):
        dirs[:] = [d for d in dirs if d not in ('build', '.git')]
        for name in sorted(files):
            if not name.endswith('.c'):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                text = f.read()
            for m in DESC.finditer(text):
                var, body = m.group(1), m.group(2)
                for need in REQUIRED:
                    if need not in body:
                        findings.append((rel, var, '缺 %s' % need))
                if ('.on_pause' in body and '.on_start' not in body
                        and '.on_resume' not in body):
                    findings.append((rel, var, '有 on_pause 但没有 on_start / on_resume'))

    if not findings:
        print('OK: App 描述符的回调搭配正确')
        return 0

    print('发现 %d 处 App 描述符问题：' % len(findings))
    for rel, var, why in findings:
        print('  %s  %s: %s' % (rel, var, why))
    return 1


if __name__ == '__main__':
    sys.exit(main())
