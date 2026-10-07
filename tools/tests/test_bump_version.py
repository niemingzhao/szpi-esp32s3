# -*- coding: utf-8 -*-
"""版本号读写（tools/bump_version.py）的测试：解析、提升、只改一行、CRLF 保持。"""

import contextlib
import io
import unittest

from helpers import load_tool, temp_repo


def silent(fn, *args, **kwargs):
    """跑一次工具函数，吞掉它的打印（测试输出干净些）。"""
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*args, **kwargs)

HEADER = (
    '/*\n'
    ' * Services - 产品标识\n'
    ' */\n'
    '\n'
    '#pragma once\n'
    '\n'
    '#define SZPI_OS_NAME      "SZPI-OS"\n'
    '\n'
    '#define SZPI_OS_VERSION   "v0.5"\n'
    '\n'
    'const char *svc_identity_ap_ssid(void);\n'
)


class VersionTest(unittest.TestCase):

    def setUp(self):
        self.tool = load_tool('bump_version.py')

    def test_parse(self):
        self.assertEqual((True, [0, 5]), self.tool.parse_version('v0.5'))
        self.assertEqual((False, [1, 2, 3]), self.tool.parse_version('1.2.3'))
        self.assertEqual((True, [10]), self.tool.parse_version('v10'))

    def test_parse_rejects_junk(self):
        for bad in ('', 'v', '1.a', 'x1.0', '1..0'):
            with self.assertRaises(ValueError, msg=bad):
                self.tool.parse_version(bad)

    def test_bump_keeps_segment_count(self):
        self.assertEqual('v0.6', self.tool.bump_version('v0.5', 'minor'))
        self.assertEqual('v1.0', self.tool.bump_version('v0.5', 'major'))
        self.assertEqual('v0.7.0', self.tool.bump_version('v0.6.3', 'minor'))
        self.assertEqual('v1.0.0', self.tool.bump_version('v0.9.3', 'major'))

    def test_bump_one_segment(self):
        self.assertEqual('v2', self.tool.bump_version('v1', 'major'))
        self.assertEqual('v1.1', self.tool.bump_version('v1', 'minor'))

    def test_bump_patch_adds_third_segment(self):
        self.assertEqual('v0.5.1', self.tool.bump_version('v0.5', 'patch'))
        self.assertEqual('v0.5.2', self.tool.bump_version('v0.5.1', 'patch'))

    def test_rewrite_only_touches_macro(self):
        out = self.tool.rewrite(HEADER, 'v0.6')
        self.assertEqual(HEADER.replace('"v0.5"', '"v0.6"'), out)

    def test_rewrite_keeps_crlf(self):
        text = HEADER.replace('\n', '\r\n')
        out = self.tool.rewrite(text, 'v1.0')
        self.assertEqual(len(text), len(out))                       # 长度不变：没有混进多余的换行
        self.assertIn('#define SZPI_OS_VERSION   "v1.0"\r\n', out)
        self.assertNotIn('\r\r', out)

    def test_read_current(self):
        self.assertEqual('v0.5', self.tool.read_current(HEADER))

    def test_rewrite_requires_exactly_one(self):
        with self.assertRaises(ValueError):
            self.tool.rewrite(HEADER + '#define SZPI_OS_VERSION   "v9.9"\n', 'v1.0')


class CliTest(unittest.TestCase):

    def _run(self, *args, text=HEADER):
        """在临时头文件上跑一遍命令，返回 (退出码, 文件内容)。"""
        with temp_repo() as root:
            path = root / 'svc_identity.h'
            path.write_text(text, encoding='utf-8', newline='')
            tool = load_tool('bump_version.py')
            ret = silent(tool.main, ['--header', str(path), *args])
            with open(path, 'r', encoding='utf-8', newline='') as f:
                return ret, f.read()

    def test_show_does_not_write(self):
        ret, text = self._run('--show')
        self.assertEqual(0, ret)
        self.assertEqual(HEADER, text)

    def test_dry_run_does_not_write(self):
        ret, text = self._run('--bump', 'minor', '--dry-run')
        self.assertEqual(0, ret)
        self.assertEqual(HEADER, text)

    def test_bump_writes(self):
        ret, text = self._run('--bump', 'minor')
        self.assertEqual(0, ret)
        self.assertIn('#define SZPI_OS_VERSION   "v0.6"', text)

    def test_set_normalizes(self):
        ret, text = self._run('--set', '1.0.0')
        self.assertEqual(0, ret)
        self.assertIn('#define SZPI_OS_VERSION   "v1.0.0"', text)

    def test_github_output(self):
        with temp_repo() as root:
            path = root / 'svc_identity.h'
            path.write_text(HEADER, encoding='utf-8', newline='')
            out = root / 'out.txt'
            tool = load_tool('bump_version.py')
            silent(tool.main, ['--header', str(path), '--bump', 'minor', '--github-output', str(out)])
            self.assertEqual('version=v0.6\n', out.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
