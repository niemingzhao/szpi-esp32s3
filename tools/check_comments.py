#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""自检：注释里不该出现的格式与引用（去 AI 味 / 无来源引用）。

只检查注释，代码与字符串字面量不参与：

  1. Markdown 加粗 **...** 与反引号：C 注释不渲染 Markdown，是纯噪声。
  2. 行首的 ">" 引用块。
  3. emoji。
  4. 引用外部文档 / 配置文件：docs/、AGENTS、sdkconfig、partitions.csv、dependencies.lock。

用法：
    python tools/check_comments.py
退出码：0 = 通过，1 = 有问题（列出文件与位置）
"""

import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SKIP_DIRS = ('managed_components', 'build', '.git', 'assets')

BOLD = re.compile(r'\*\*[^*\n]+\*\*')
BACKTICK = re.compile(r'`')
EMOJI = re.compile('[\U0001F000-\U0001FAFF\U00002600-\U000027BF\U00002B00-\U00002BFF\uFE0F]')
REF = re.compile(r'docs/|AGENTS|sdkconfig|partitions\.csv|dependencies\.lock')


def comments_only(text):
    """返回与原文同长度的串：注释原样保留，其余字符换成空格（换行保留）。

    这样逐行检查时行号与原文一致，又不会把代码里的 `**`（比如指针）或字符串里的
    反引号当成注释内容。
    """
    out = []
    i, n = 0, len(text)
    state = 'code'
    while i < n:
        c = text[i]
        if state == 'code':
            if c == '/' and i + 1 < n and text[i + 1] == '/':
                state = 'line'; out.append('//'); i += 2; continue
            if c == '/' and i + 1 < n and text[i + 1] == '*':
                state = 'block'; out.append('/*'); i += 2; continue
            if c == '"':
                state = 'str'; out.append(' '); i += 1; continue
            if c == "'":
                state = 'chr'; out.append(' '); i += 1; continue
            out.append('\n' if c == '\n' else ' '); i += 1; continue
        if state == 'line':
            if c == '\n':
                state = 'code'; out.append('\n'); i += 1; continue
            out.append(c); i += 1; continue
        if state == 'block':
            if c == '*' and i + 1 < n and text[i + 1] == '/':
                state = 'code'; out.append('  '); i += 2; continue
            out.append(c); i += 1; continue
        # str / chr
        if c == '\\':
            out.append('  '); i += 2; continue
        out.append('\n' if c == '\n' else ' ')
        if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
            state = 'code'
        i += 1
    return ''.join(out)


def is_blockquote(line):
    """判断注释标记之后是不是 ">" 引用块：块注释续行 "* > ..." 或行注释 "// > ..."。
    只看标记之后，避免把 "> 0" 这种比较写法的注释误判。"""
    s = line.strip()
    if s.startswith('//'):
        s = s[2:].lstrip()
    elif s.startswith('*') and not s.startswith('*/'):
        s = s[1:].lstrip()
    else:
        return False
    return s.startswith('>')


def check_file(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        view = comments_only(f.read())

    hits = []
    for lineno, line in enumerate(view.split('\n'), 1):
        if not line.strip():
            continue
        text = line.strip()
        if BOLD.search(line):
            hits.append((lineno, 'Markdown 加粗 **...**', text))
        if BACKTICK.search(line):
            hits.append((lineno, '反引号', text))
        if EMOJI.search(line):
            hits.append((lineno, 'emoji', text))
        if REF.search(line):
            hits.append((lineno, '引用外部文档 / 配置文件', text))
        if is_blockquote(line):
            hits.append((lineno, '">" 引用块', text))
    return hits


def main():
    findings = []
    for root, dirs, files in os.walk(os.path.join(ROOT, 'main')):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in sorted(files):
            if not name.endswith(('.c', '.h')):
                continue
            p = os.path.join(root, name)
            rel = os.path.relpath(p, ROOT).replace('\\', '/')
            for lineno, why, text in check_file(p):
                findings.append((rel, lineno, why, text))

    if not findings:
        print('OK: 注释没有 Markdown 格式 / 外部文档引用 / emoji')
        return 0

    print('发现 %d 处注释问题：' % len(findings))
    for path, lineno, why, text in findings:
        print('  %s:%d  %s' % (path, lineno, why))
        print('      %s' % text[:100])
    return 1


if __name__ == '__main__':
    sys.exit(main())
