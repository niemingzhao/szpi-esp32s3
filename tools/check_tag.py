#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：每个 .c 都有且只有一个 TAG，且 TAG 前缀与所在层一致。

约定（TAG 带层前缀）：
  main/drivers      drv. / bsp.
  main/peripherals  periph.
  main/services     svc.
  main/framework    fw.
  main/apps         app.
  main/main.c       szpi-os

生成的资源文件（framework/assets/）没有 TAG，不参与检查。

用法：
    python tools/check_tag.py
退出码：0 = 通过，1 = 有问题（列出文件）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SKIP_DIRS = ('managed_components', 'build', '.git', 'assets')

TAG = re.compile(r'static\s+const\s+char\s*\*\s*TAG\s*=\s*"([^"]+)"')

PREFIX = {
    'drivers': ('drv.', 'bsp.'),
    'peripherals': ('periph.',),
    'services': ('svc.',),
    'framework': ('fw.',),
    'apps': ('app.',),
}


def expected_prefix(rel):
    if rel == 'main/main.c':
        return ('szpi-os',)
    parts = rel.split('/')
    if len(parts) >= 2:
        return PREFIX.get(parts[1])
    return None


def main():
    findings = []
    for root, dirs, files in os.walk(os.path.join(ROOT, 'main')):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in sorted(files):
            if not name.endswith('.c'):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            prefixes = expected_prefix(rel)
            if prefixes is None:
                continue
            with open(p, 'r', encoding='utf-8', errors='ignore') as f:
                tags = TAG.findall(f.read())
            if len(tags) != 1:
                findings.append((rel, '有 %d 个 TAG（应恰好 1 个）' % len(tags)))
            elif not any(tags[0].startswith(x) for x in prefixes):
                findings.append((rel, 'TAG "%s" 前缀应为 %s' % (tags[0], ' / '.join(prefixes))))

    if not findings:
        print('OK: 每个 .c 都有 TAG，前缀与所在层一致')
        return 0

    print('发现 %d 处 TAG 问题：' % len(findings))
    for rel, why in findings:
        print('  %s: %s' % (rel, why))
    return 1


if __name__ == '__main__':
    sys.exit(main())
