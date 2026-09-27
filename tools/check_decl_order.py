#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：文件内 static 函数/变量的"定义晚于使用"（编译会报 undeclared）。

背景：把一段 static 代码块挪到别处时，很容易出现"定义在下面、使用在上面"，
编译器报 undeclared identifier（GCC 15 下还会连带报 conflicting types / 
static declaration follows non-static declaration）。这个脚本就是查这个。

只有文件顶部写了 static 函数原型（前置声明）才放行；调用语句（以 ; 结尾）
不算前置声明 —— 这一点很容易写错，所以这里先去掉注释与字符串字面量，再逐行找名字。

用法：
    python tools/check_decl_order.py
退出码：0 = 通过，1 = 可疑（列出文件与位置）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
LAYERS = ('main',)
SKIP_DIRS = ('managed_components', 'build', '.git', 'tools', 'docs', 'assets')

FUNC_DEF = re.compile(r'(?m)^static\s+[A-Za-z_][\w \*]*?\s+([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*$')
VAR_DEF = re.compile(r'(?m)^static\s+[A-Za-z_][\w \*]*?\s+([a-z_][a-z0-9_]*)\s*(?:=|;)')
# 前置声明：static 函数原型（以 ; 结尾）单独一行，允许出现在使用之前
FUNC_DECL = re.compile(r'(?m)^static\s+[A-Za-z_][\w \*]*?\s+([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*;\s*$')


def blank_comments_and_literals(text):
    """把注释与字符串/字符字面量的内容换成空格（保留行列数，便于定位）。

    返回的文本里，注释与字面量不会再被当成标识符匹配到。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]

        if ch == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join('\n' if c == '\n' else ' ' for c in text[i:j]))
            i = j
        elif ch == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif ch in ('"', "'"):
            quote = ch
            j = i + 1
            while j < n and text[j] != quote:
                if text[j] == '\\':
                    j += 1
                j += 1
            j = min(j + 1, n)
            # 保留两头的引号，内容清空
            body = text[i:j]
            out.append(quote + ' ' * max(len(body) - 2, 0) + quote if len(body) >= 2 else ' ')
            i = j
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


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
                    lines = blank_comments_and_literals(fh.read()).splitlines()

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
                        if not pat.search(ln) or ln.lstrip().startswith('#'):
                            continue
                        # 文件里在本行之前（含本行，即原型本身）有 static 原型才放行
                        if name in decls and decls[name] <= i:
                            continue
                        findings.append((os.path.relpath(p, ROOT).replace('\\', '/'), i + 1, def_line + 1, name))
                        break

    if not findings:
        print('OK: static 定义都在使用之前')
        return 0

    print('发现 %d 处可能"定义晚于使用"（编译会报 undeclared）：' % len(findings))
    for path, use, dfn, name in sorted(findings):
        print('  %s:%d 使用了 %s（定义在 %d 行）' % (path, use, name, dfn))
    print('\n把定义上移，或在文件顶部加前置声明（static 函数原型 / 变量请上移）。')
    return 1


if __name__ == '__main__':
    sys.exit(main())
