#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读写系统版本号（`main/services/include/svc_identity.h` 里的 SZPI_OS_VERSION）。

版本号全工程只定义一次，就在这里；发布流程用它改版本号：

    python tools/bump_version.py --show                  # 只打印当前版本
    python tools/bump_version.py --bump patch            # v0.5 -> v0.5.1
    python tools/bump_version.py --bump minor            # v0.5 -> v0.6
    python tools/bump_version.py --bump major            # v0.5 -> v1.0
    python tools/bump_version.py --set 1.0.0             # 直接指定（可省略前导 v）
    python tools/bump_version.py --bump minor --dry-run  # 只打印，不写文件

只改宏定义那一行，行尾（LF / CRLF）、对齐与文件其余内容原样保留。版本号保留当前的分段数：
`v0.5` 是两段，major / minor 之后还是两段；patch 需要第三段时自动补上（`v0.5` -> `v0.5.1`）。

在 CI 里跑时会把 `version=<新版本>` 追加到 `$GITHUB_OUTPUT`（或用 --github-output 指定文件）。
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, 'main', 'services', 'include', 'svc_identity.h')

LINE = re.compile(r'(?P<pre>^[ \t]*#define[ \t]+SZPI_OS_VERSION[ \t]+)"(?P<ver>v?\d+(?:\.\d+)*)"',
                  re.M)


def parse_version(text):
    """拆出版本号，返回 (是否带前导 v, 各段整数)。格式不对抛 ValueError。"""
    m = re.fullmatch(r'(v?)(\d+(?:\.\d+)*)', text.strip())
    if m is None:
        raise ValueError('版本号格式不对：%s（应为 v0.5 / 0.5 / 1.0.0 这类）' % text)
    return bool(m.group(1)), [int(x) for x in m.group(2).split('.')]


def format_version(has_v, parts):
    return ('v' if has_v else '') + '.'.join(str(p) for p in parts)


def bump_version(current, kind):
    """按 major / minor / patch 算出下一个版本，分段数与当前一致（patch 需要时补第三段）。"""
    has_v, parts = parse_version(current)

    if kind == 'major':
        parts = [parts[0] + 1] + [0] * (len(parts) - 1)
    elif kind == 'minor':
        while len(parts) < 2:
            parts.append(0)
        parts = [parts[0], parts[1] + 1] + [0] * (len(parts) - 2)
    elif kind == 'patch':
        while len(parts) < 3:
            parts.append(0)
        parts[2] += 1
    else:
        raise ValueError('未知的提升方式：%s' % kind)

    return format_version(has_v, parts)


def read_current(text):
    """返回头文件文本里的当前版本号。"""
    m = LINE.search(text)
    if m is None:
        raise ValueError('在头文件里找不到 SZPI_OS_VERSION')
    return m.group('ver')


def rewrite(text, version):
    """把宏定义的值换成 version，返回新文本（只改那一行）。"""
    new, n = LINE.subn(lambda m: '%s"%s"' % (m.group('pre'), version), text)
    if n != 1:
        raise ValueError('头文件里应当恰好有一个 SZPI_OS_VERSION（找到 %d 个）' % n)
    return new


def main(argv=None):
    ap = argparse.ArgumentParser(description='读写系统版本号（svc_identity.h 的 SZPI_OS_VERSION）')
    ap.add_argument('--show', action='store_true', help='只打印当前版本')
    ap.add_argument('--bump', choices=('major', 'minor', 'patch'), help='按语义化版本提升')
    ap.add_argument('--set', dest='set_to', metavar='X.Y.Z', help='直接指定版本（可省略前导 v）')
    ap.add_argument('--dry-run', action='store_true', help='只打印结果，不写文件')
    ap.add_argument('--header', metavar='FILE', help='头文件路径（默认 svc_identity.h，测试用）')
    ap.add_argument('--github-output', metavar='FILE',
                    help='把 version=... 追加到这个文件（默认取环境变量 GITHUB_OUTPUT）')
    args = ap.parse_args(argv)

    header = args.header or HEADER
    with open(header, 'r', encoding='utf-8', newline='') as f:
        text = f.read()
    current = read_current(text)

    if args.show:
        version = current
    elif args.bump:
        version = bump_version(current, args.bump)
    elif args.set_to:
        _, parts = parse_version(args.set_to)
        cur_has_v, _ = parse_version(current)          # 前导 v 跟文件现有写法保持一致
        version = format_version(cur_has_v, parts)
    else:
        ap.error('需要 --show / --bump / --set 三者之一')

    if args.show:
        print(version)                                  # 只打印版本号，方便脚本直接取用
    elif version == current:
        print('%s（版本没变，未写入）' % version)
    elif args.dry_run:
        print('%s -> %s（--dry-run，未写入 %s）' % (current, version, header))
    else:
        with open(header, 'w', encoding='utf-8', newline='') as f:
            f.write(rewrite(text, version))
        print('%s -> %s（已写入 %s）' % (current, version, header))

    out = args.github_output or os.environ.get('GITHUB_OUTPUT')
    if out:
        with open(out, 'a', encoding='utf-8', newline='') as f:
            f.write('version=%s\n' % version)
    return 0


if __name__ == '__main__':
    sys.exit(main())
