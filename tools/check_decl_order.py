#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：文件内 static 函数/变量的"定义晚于使用"（编译会报 undeclared）。

背景：把一段 static 代码块挪到别处时，很容易出现"定义在下面、使用在上面"，
编译器报 undeclared identifier。C 语言里除了前置声明（允许）之外，定义必须在
第一次使用之前，这个脚本就是查这个。

用法：
    python tools/check_decl_order.py
退出码：0 = 通过，1 = 可疑（列出文件与位置，需人工确认，可能是注释里的引用）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
LAYERS = ('main',)
SKIP_DIRS = ('managed_components', 'build', '.git', 'tools', 'docs')

FUNC_DEF = re.compile(r'(?m)^static\s+[A-Za-z_][\w \*]*?\s+([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*$')
VAR_DEF = re.compile(r'(?m)^static\s+[A-Za-z_][\w \*]*?\s+([a-z_][a-z0-9_]*)\s*(?:=|;)')
# 前置声明：static 函数原型（以 ; 结尾）单独一行，允许出现在使用之前
FUNC_DECL = re.compile(r'(?m)^static\s+[A-Za-z_][\w \*]*?\s+([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*;\s*$')


def is_comment_line(ln):
    s = ln.strip()
    return s.startswith(('*', '/*', '//', '*/'))


def main():
    findings = []
    for layer in LAYERS:
        base = os.path.join(ROOT, layer)
        for root, dirs, files in os.walk(base):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in sorted(files):
                if not f.endswith('.c'):
                    continue
                p = os.path.join(root, f)
                with open(p, 'r', encoding='utf-8', errors='ignore') as fh:
                    lines = fh.read().splitlines()

                defs = {}
                decls = {}
                for i, ln in enumerate(lines):
                    m = FUNC_DEF.match(ln) or VAR_DEF.match(ln)
                    if m:
                        defs.setdefault(m.group(1), i)
                    d = FUNC_DECL.match(ln)
                    if d:
                        decls.setdefault(d.group(1), i)

                for name, def_line in defs.items():
                    pat = re.compile(r'(?<![a-zA-Z_])' + re.escape(name) + r'\b')
                    for i in range(0, def_line):
                        ln = lines[i]
                        if not pat.search(ln) or is_comment_line(ln) or ln.lstrip().startswith('#'):
                            continue
                        # 前置声明（以 ; 结尾且不是定义）是允许的
                        if ln.rstrip().endswith(';') and '(' in ln:
                            continue
                        # 文件里在本行之前有 static 原型，也允许（多行调用会导致上面的启发式失效）
                        if name in decls and decls[name] < i:
                            continue
                        findings.append((os.path.relpath(p, ROOT).replace('\\', '/'), i + 1, def_line + 1, name))
                        break

    if not findings:
        print('OK: static 定义都在使用之前')
        return 0

    print('发现 %d 处可能"定义晚于使用"（编译会报 undeclared）：' % len(findings))
    for path, use, dfn, name in sorted(findings):
        print('  %s:%d 使用了 %s（定义在 %d 行）' % (path, use, name, dfn))
    print('\n把定义上移，或在文件顶部加前置声明（static 函数原型 / extern 变量）。')
    return 1


if __name__ == '__main__':
    sys.exit(main())
