#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：术语写法是否统一。

按项目约定：
  I2C（不写 I²C）、Wi-Fi（不写 WiFi）、TF 卡（不写 SD 卡）、SPIFFS（不写 LittleFS）。

注释与字符串都算（界面文案、日志也要求同样的写法）。

用法：
    python tools/check_terms.py
退出码：0 = 通过，1 = 有不合规写法（列出文件与位置）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SKIP_DIRS = ('managed_components', 'build', '.git', 'assets')

RULES = (
    (re.compile(r'I\s*²\s*C'), 'I²C 应为 I2C'),
    (re.compile(r'(?<![A-Za-z-])WiFi(?![A-Za-z])'), 'WiFi 应为 Wi-Fi'),
    (re.compile(r'SD\s*卡'), 'SD 卡应为 TF 卡'),
    (re.compile(r'(?i)littlefs'), 'LittleFS 应为 SPIFFS'),
)


def main():
    findings = []
    for root, dirs, files in os.walk(os.path.join(ROOT, 'main')):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in sorted(files):
            if not name.endswith(('.c', '.h')):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                for lineno, line in enumerate(f, 1):
                    for rx, why in RULES:
                        if rx.search(line):
                            findings.append((rel, lineno, why, line.strip()[:100]))

    if not findings:
        print('OK: 术语写法统一（I2C / Wi-Fi / TF 卡 / SPIFFS）')
        return 0

    print('发现 %d 处术语写法不合规：' % len(findings))
    for path, lineno, why, text in findings:
        print('  %s:%d  %s' % (path, lineno, why))
        print('      %s' % text)
    return 1


if __name__ == '__main__':
    sys.exit(main())
