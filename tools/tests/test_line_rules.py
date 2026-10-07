# -*- coding: utf-8 -*-
"""逐条验证"按行/按调用"的静态自检：坏样例必须报错，好样例必须放行。

这些自检都在临时仓库上跑（把模块的 ROOT 指过去），所以不会动真实代码。
"""

import unittest

from helpers import call_main, load_tool, point_to, temp_repo, write


def check(name, root):
    """在临时仓库上跑一个自检，返回退出码。"""
    return call_main(point_to(load_tool(name), root))[0]


class TermsTest(unittest.TestCase):

    def test_rejects_wifi(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', '// 用 WiFi 连\n')
            self.assertEqual(1, check('check_terms.py', root))

    def test_accepts_wi_fi(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', '// 用 Wi-Fi 连\n')
            self.assertEqual(0, check('check_terms.py', root))

    def test_ignores_non_source(self):
        """只扫 .c / .h，其它文件的写法不算数。"""
        with temp_repo() as root:
            write(root, 'main/drivers/src/notes.txt', 'WiFi\n')
            self.assertEqual(0, check('check_terms.py', root))


class CommentsTest(unittest.TestCase):

    def test_rejects_markdown_bold(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', '/* 注意 **这里是加粗** */\n')
            self.assertEqual(1, check('check_comments.py', root))

    def test_rejects_doc_reference(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', '/* 详见 docs/xx */\n')
            self.assertEqual(1, check('check_comments.py', root))

    def test_rejects_blockquote(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', '/*\n * > 引用\n */\n')
            self.assertEqual(1, check('check_comments.py', root))

    def test_accepts_plain_comment(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', '/* 普通注释，可以带 == 与 -> */\n')
            self.assertEqual(0, check('check_comments.py', root))

    def test_ignores_string_literal(self):
        """字符串里的反引号是正常的界面文案，不算注释问题。"""
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c',
                  'static const char *s = "命令是 `ls`";\n')
            self.assertEqual(0, check('check_comments.py', root))


class PrintfTest(unittest.TestCase):

    def test_rejects_printf(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', 'void f(void) { printf("hi"); }\n')
            self.assertEqual(1, check('check_printf.py', root))

    def test_rejects_puts(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', 'void f(void) { puts("hi"); }\n')
            self.assertEqual(1, check('check_printf.py', root))

    def test_accepts_esp_log(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c',
                  'void f(void) { ESP_LOGI(TAG, "hi"); snprintf(b, n, "x"); }\n')
            self.assertEqual(0, check('check_printf.py', root))


class TagTest(unittest.TestCase):

    def test_accepts_layer_prefix(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', 'static const char *TAG = "drv.demo";\n')
            self.assertEqual(0, check('check_tag.py', root))

    def test_rejects_wrong_prefix(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', 'static const char *TAG = "app.demo";\n')
            self.assertEqual(1, check('check_tag.py', root))

    def test_rejects_missing_tag(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', 'void f(void) {}\n')
            self.assertEqual(1, check('check_tag.py', root))

    def test_rejects_two_tags(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c',
                  'static const char *TAG = "drv.a";\nstatic const char *TAG = "drv.b";\n')
            self.assertEqual(1, check('check_tag.py', root))

    def test_main_c_uses_szpi_os(self):
        with temp_repo() as root:
            write(root, 'main/main.c', 'static const char *TAG = "szpi-os";\n')
            self.assertEqual(0, check('check_tag.py', root))


class DeclOrderTest(unittest.TestCase):

    BAD = (
        'static void caller(void)\n'
        '{\n'
        '    helper();\n'
        '}\n'
        '\n'
        'static void helper(void)\n'
        '{\n'
        '}\n'
    )
    GOOD = (
        'static void helper(void)\n'
        '{\n'
        '}\n'
        '\n'
        'static void caller(void)\n'
        '{\n'
        '    helper();\n'
        '}\n'
    )

    def test_rejects_use_before_definition(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', self.BAD)
            self.assertEqual(1, check('check_decl_order.py', root))

    def test_accepts_definition_first(self):
        with temp_repo() as root:
            write(root, 'main/drivers/src/demo.c', self.GOOD)
            self.assertEqual(0, check('check_decl_order.py', root))


class ApiIncludesTest(unittest.TestCase):

    HEADER = 'esp_err_t svc_demo_ping(int x);\n'

    def test_rejects_call_without_header(self):
        with temp_repo() as root:
            write(root, 'main/services/include/svc_demo.h', self.HEADER)
            write(root, 'main/apps/app_demo/app_demo.c', 'void f(void) { svc_demo_ping(1); }\n')
            self.assertEqual(1, check('check_api_includes.py', root))

    def test_accepts_call_with_header(self):
        with temp_repo() as root:
            write(root, 'main/services/include/svc_demo.h', self.HEADER)
            write(root, 'main/apps/app_demo/app_demo.c',
                  '#include "svc_demo.h"\nvoid f(void) { svc_demo_ping(1); }\n')
            self.assertEqual(0, check('check_api_includes.py', root))


if __name__ == '__main__':
    unittest.main()
