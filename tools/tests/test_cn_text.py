# -*- coding: utf-8 -*-
"""验证中文字体覆盖自检：字符能不能渲染、字体 cmap 是否"含端点"。"""

import unittest

from helpers import call_main, load_tool, point_to, temp_repo, write

EMOJI = chr(0x1F600)


class CoverageTest(unittest.TestCase):

    def setUp(self):
        self.tool = load_tool('check_cn_text.py')

    def test_ascii_covered(self):
        self.assertTrue(self.tool.is_covered('A'))
        self.assertTrue(self.tool.is_covered(' '))

    def test_gbk_covered(self):
        self.assertTrue(self.tool.is_covered('中'))
        self.assertTrue(self.tool.is_covered('，'))

    def test_emoji_not_covered(self):
        """emoji 不在 GBK 里，应当被判定为覆盖不到。"""
        self.assertFalse(self.tool.is_covered(EMOJI))


CMAP_OK = (
    '    .range_start = 32, .range_length = 2, .glyph_id_start = 1,\n'
    '        .unicode_list = demo_list,\n'
    'static const uint16_t demo_list[] = {0x0000, 0x0001};\n'
)

# range_length 写成 last - first（少 1），码点最大的那个字会查不到字形
CMAP_BAD = (
    '    .range_start = 32, .range_length = 1, .glyph_id_start = 1,\n'
    '        .unicode_list = demo_list,\n'
    'static const uint16_t demo_list[] = {0x0000, 0x0001};\n'
)


class FontCmapTest(unittest.TestCase):

    def _run(self, text):
        with temp_repo() as root:
            tool = point_to(load_tool('check_cn_text.py'), root)
            p = root / 'main' / 'framework' / 'assets' / 'fw_fonts.c'
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding='utf-8')
            return tool.check_font_cmap(p)

    def test_accepts_inclusive_range(self):
        ok, msg = self._run(CMAP_OK)
        self.assertTrue(ok, msg)

    def test_rejects_exclusive_range(self):
        ok, msg = self._run(CMAP_BAD)
        self.assertFalse(ok, msg)


class ScanTest(unittest.TestCase):

    def test_finds_uncovered_char(self):
        with temp_repo() as root:
            tool = point_to(load_tool('check_cn_text.py'), root)
            write(root, 'main/apps/app_demo/app_demo.c',
                  'static const char *s = "ok %s";\n' % EMOJI)
            self.assertIn(EMOJI, tool.collect_missing())

    def test_ignores_comment_chars(self):
        """注释里的字符不参与检查（只有字符串字面量算文案）。"""
        with temp_repo() as root:
            tool = point_to(load_tool('check_cn_text.py'), root)
            write(root, 'main/apps/app_demo/app_demo.c', '/* %s */\n' % EMOJI)
            self.assertEqual({}, tool.collect_missing())

    def test_main_returns_zero_when_clean(self):
        with temp_repo() as root:
            tool = point_to(load_tool('check_cn_text.py'), root)
            write(root, 'main/apps/app_demo/app_demo.c', 'static const char *s = "中文";\n')
            p = root / 'main' / 'framework' / 'assets' / 'fw_fonts.c'
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(CMAP_OK, encoding='utf-8')
            ret, _ = call_main(tool)
            self.assertEqual(0, ret)


if __name__ == '__main__':
    unittest.main()
