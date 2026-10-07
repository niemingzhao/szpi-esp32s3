# -*- coding: utf-8 -*-
"""资源生成脚本的测试：RGB565 字节序，以及"生成结果与仓库里的资源一致"。

图标生成很快，默认就跑；字体生成要栅格化两万多字，很慢，用环境变量 SZPI_TEST_FONTS=1
打开。两者都是把生成脚本复制到临时仓库里跑，不会覆盖仓库里的资源。
"""

import os
import shutil
import subprocess
import sys
import unittest

from helpers import ROOT, TOOLS, load_tool, temp_repo

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:                                    # 本地没装 pillow 就跳过
    HAVE_PIL = False


def regen(name, outputs, extra_files=()):
    """把 tools/<name> 复制到临时仓库里跑一遍，返回 {相对路径: 生成的内容}。"""
    with temp_repo() as root:
        (root / 'tools').mkdir(exist_ok=True)
        shutil.copy(TOOLS / name, root / 'tools' / name)
        for rel in extra_files:
            dst = root / 'tools' / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(TOOLS / rel, dst)
        for rel in outputs:
            (root / rel).parent.mkdir(parents=True, exist_ok=True)

        proc = subprocess.run([sys.executable, str(root / 'tools' / name)],
                              cwd=str(root), capture_output=True, text=True,
                              encoding='utf-8', errors='replace',
                              env=dict(os.environ, PYTHONIOENCODING='utf-8'))
        if proc.returncode != 0:
            raise AssertionError('%s 运行失败：\n%s%s' % (name, proc.stdout, proc.stderr))
        return {rel: (root / rel).read_bytes() for rel in outputs}


def normalize(data):
    """行尾统一成 LF 再比较：生成脚本写 LF，而 Windows 检出会把仓库文件变成 CRLF。"""
    return data.replace(b'\r\n', b'\n')


ICON_OUTPUTS = (
    'main/framework/assets/fw_icons.c',
    'main/framework/include/fw_icons.h',
)

FONT_OUTPUTS = (
    'main/framework/assets/fw_fonts.c',
    'main/framework/include/fw_fonts.h',
)


class IconEncodingTest(unittest.TestCase):

    @unittest.skipUnless(HAVE_PIL, '需要 pillow')
    def test_rgb565_is_little_endian(self):
        """纯红 0xF800 必须按小端写成 00 F8（写屏回调会再交换一次）。"""
        gen = load_tool('gen_fw_icons.py')
        gen.set_context(1)
        try:
            rgb, a8 = gen.encode_a8(Image.new('RGBA', (1, 1), (255, 0, 0, 128)))
        finally:
            gen.set_context(20)
        self.assertEqual(b'\x00\xf8', rgb)
        self.assertEqual(b'\x80', a8)


class ReproducibleTest(unittest.TestCase):

    @unittest.skipUnless(HAVE_PIL, '需要 pillow')
    def test_icons_match_repo(self):
        """仓库里的图标资源必须与重新生成的结果一致（改了绘图代码要重跑生成脚本）。"""
        for rel, data in regen('gen_fw_icons.py', ICON_OUTPUTS).items():
            self.assertEqual(normalize((ROOT / rel).read_bytes()), normalize(data),
                             '%s 与重新生成的不一致，请跑 python tools/gen_fw_icons.py' % rel)

    @unittest.skipUnless(HAVE_PIL, '需要 pillow')
    @unittest.skipUnless(os.environ.get('SZPI_TEST_FONTS'), '设 SZPI_TEST_FONTS=1 才跑（较慢）')
    def test_fonts_match_repo(self):
        """仓库里的字体资源必须与重新生成的结果一致（改字符集要重跑生成脚本）。"""
        out = regen('gen_fw_fonts.py', FONT_OUTPUTS,
                    extra_files=(os.path.join('fonts', 'NotoSansSC-VF.ttf'),))
        for rel, data in out.items():
            self.assertEqual(normalize((ROOT / rel).read_bytes()), normalize(data),
                             '%s 与重新生成的不一致，请跑 python tools/gen_fw_fonts.py' % rel)


if __name__ == '__main__':
    unittest.main()
