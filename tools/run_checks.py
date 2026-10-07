#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""依次运行 tools/ 下的全部自检脚本（check_*.py）。

CI、pre-commit 钩子与本地手工检查共用它，保证"跑的是同一套检查"：
全部通过退出码 0，任一失败退出码 1。

用法：
    python tools/run_checks.py
"""

import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))


def main():
    scripts = sorted(n for n in os.listdir(TOOLS)
                     if n.startswith('check_') and n.endswith('.py'))

    env = dict(os.environ, PYTHONIOENCODING='utf-8')
    failed = []
    for name in scripts:
        print('==> %s' % name, flush=True)
        ret = subprocess.run([sys.executable, os.path.join(TOOLS, name)], env=env).returncode
        if ret != 0:
            failed.append(name)

    print(flush=True)
    if failed:
        print('自检未通过（%d/%d）：%s' % (len(failed), len(scripts), ', '.join(failed)), flush=True)
        return 1
    print('自检全部通过（%d 项）' % len(scripts), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
