# -*- coding: utf-8 -*-
"""验证界面与 App 结构类自检（标签配色、App 注册表、App 回调搭配）。"""

import unittest

from helpers import call_main, load_tool, point_to, temp_repo, write


def check(name, root):
    return call_main(point_to(load_tool(name), root))[0]


class UiColorsTest(unittest.TestCase):

    def test_rejects_label_without_color(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c',
                  'void f(lv_obj_t *p)\n'
                  '{\n'
                  '    lv_obj_t *lbl = lv_label_create(p);\n'
                  '    lv_label_set_text(lbl, "x");\n'
                  '}\n')
            self.assertEqual(1, check('check_ui_colors.py', root))

    def test_accepts_label_with_color(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c',
                  'void f(lv_obj_t *p)\n'
                  '{\n'
                  '    lv_obj_t *lbl = lv_label_create(p);\n'
                  '    lv_obj_set_style_text_color(lbl, fw_theme_color_text(), 0);\n'
                  '}\n')
            self.assertEqual(0, check('check_ui_colors.py', root))


APP_SRC_OK = (
    'const fw_app_desc_t g_demo_app = {\n'
    '    .name = "Demo",\n'
    '    .title = "演示",\n'
    '    .on_create = demo_on_create,\n'
    '};\n'
)


class AppRegistryTest(unittest.TestCase):

    def test_rejects_unregistered(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c', APP_SRC_OK)
            write(root, 'main/apps/src/app_common.c', 'void reg(void) {}\n')
            self.assertEqual(1, check('check_app_registry.py', root))

    def test_rejects_register_without_definition(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c', 'void f(void) {}\n')
            write(root, 'main/apps/src/app_common.c',
                  'void reg(void) { fw_app_mgr_register(&g_demo_app); }\n')
            self.assertEqual(1, check('check_app_registry.py', root))

    def test_accepts_matching(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c', APP_SRC_OK)
            write(root, 'main/apps/src/app_common.c',
                  'void reg(void) { fw_app_mgr_register(&g_demo_app); }\n')
            self.assertEqual(0, check('check_app_registry.py', root))


class AppCallbacksTest(unittest.TestCase):

    def test_rejects_missing_on_create(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c',
                  'const fw_app_desc_t g_demo_app = {\n'
                  '    .name = "Demo",\n'
                  '    .title = "演示",\n'
                  '};\n')
            self.assertEqual(1, check('check_app_callbacks.py', root))

    def test_rejects_pause_without_resume(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c',
                  'const fw_app_desc_t g_demo_app = {\n'
                  '    .name = "Demo",\n'
                  '    .title = "演示",\n'
                  '    .on_create = demo_on_create,\n'
                  '    .on_pause = demo_on_pause,\n'
                  '};\n')
            self.assertEqual(1, check('check_app_callbacks.py', root))

    def test_accepts_complete(self):
        with temp_repo() as root:
            write(root, 'main/apps/app_demo/app_demo.c',
                  'const fw_app_desc_t g_demo_app = {\n'
                  '    .name = "Demo",\n'
                  '    .title = "演示",\n'
                  '    .on_create = demo_on_create,\n'
                  '    .on_pause = demo_on_pause,\n'
                  '    .on_resume = demo_on_resume,\n'
                  '};\n')
            self.assertEqual(0, check('check_app_callbacks.py', root))


if __name__ == '__main__':
    unittest.main()
