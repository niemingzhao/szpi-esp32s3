#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：App 描述符与注册表是否对得上。

  - 每个 app_<x>/app_<x>.c 里定义的 fw_app_desc_t 都要在 app_common.c 里注册；
  - app_common.c 注册的每个描述符都要有对应的定义；
  - 注册数量 = 定义数量。

新增 App 时最容易漏的就是 app_common.c 里的 register 那一行（或者反过来多写一行）。

用法：
    python tools/check_app_registry.py
退出码：0 = 通过，1 = 对不上（列出差异）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
APPS = os.path.join(ROOT, 'main', 'apps')

DESC = re.compile(r'const\s+fw_app_desc_t\s+(\w+)\s*=')
REG = re.compile(r'fw_app_mgr_register\s*\(\s*&(\w+)\s*\)')


def main():
    defined = {}
    for root, dirs, files in os.walk(APPS):
        dirs[:] = [d for d in dirs if d not in ('build', '.git')]
        for name in sorted(files):
            if not name.endswith('.c'):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                for var in DESC.findall(f.read()):
                    defined[var] = rel

    reg_file = os.path.join(APPS, 'src', 'app_common.c')
    if not os.path.isfile(reg_file):
        print('找不到 %s' % reg_file)
        return 1
    with open(reg_file, 'r', encoding='utf-8', errors='ignore') as f:
        registered = REG.findall(f.read())

    findings = []
    for var in sorted(defined):
        if var not in registered:
            findings.append('定义在 %s 的 %s 没在 app_common.c 里注册' % (defined[var], var))
    for var in sorted(set(registered)):
        if var not in defined:
            findings.append('app_common.c 注册的 %s 找不到定义' % var)
    if len(registered) != len(defined):
        findings.append('注册数 %d 与定义数 %d 不一致' % (len(registered), len(defined)))

    if not findings:
        print('OK: %d 个 App 的定义与注册一一对应' % len(defined))
        return 0

    print('发现 %d 处 App 注册不一致：' % len(findings))
    for f in findings:
        print('  %s' % f)
    return 1


if __name__ == '__main__':
    sys.exit(main())
