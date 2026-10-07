# -*- coding: utf-8 -*-
"""真实仓库上跑一遍全部自检：这是 CI 与本地的主门槛。

自检脚本退出码就是结论：0 = 通过。任何一项不为 0，这条用例就失败并带上原始输出。
"""

import unittest

from helpers import CHECK_SCRIPTS, run_script


class RepoChecksTest(unittest.TestCase):

    def test_check_scripts_present(self):
        """tools/ 下应当有一批 check_*.py（少一个就说明被误删）。"""
        self.assertGreaterEqual(len(CHECK_SCRIPTS), 12, CHECK_SCRIPTS)

    def test_every_check_passes(self):
        """每个自检脚本在真实仓库上都必须通过。"""
        failures = []
        for name in CHECK_SCRIPTS:
            proc = run_script(name)
            if proc.returncode != 0:
                failures.append('%s -> %d\n%s' % (name, proc.returncode, proc.stdout + proc.stderr))
        self.assertEqual([], failures, '\n'.join(failures))

    def test_runner_passes(self):
        """汇聚脚本 run_checks.py 也要返回 0（CI / pre-commit 用的就是它）。"""
        proc = run_script('run_checks.py')
        self.assertEqual(0, proc.returncode, proc.stdout + proc.stderr)


if __name__ == '__main__':
    unittest.main()
