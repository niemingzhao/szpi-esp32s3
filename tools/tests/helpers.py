# -*- coding: utf-8 -*-
"""工具测试的公共辅助：子进程跑自检脚本，或把自检模块指向临时仓库。

自检脚本都读模块级的 ROOT（由脚本自身位置推出来），所以测试只要把 ROOT 指到
一个临时目录，就能用最小样例验证"该报的报、该过的过"，不需要碰真实仓库。
"""

import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools'

# 所有自检脚本（check_*.py），按名字排序
CHECK_SCRIPTS = sorted(p.name for p in TOOLS.glob('check_*.py'))


def run_script(name, *args):
    """在仓库根目录以子进程运行 tools/<name>，返回 CompletedProcess。"""
    return subprocess.run(
        [sys.executable, str(TOOLS / name), *args],
        cwd=str(ROOT), capture_output=True, text=True,
        encoding='utf-8', errors='replace',
        env=dict(os.environ, PYTHONIOENCODING='utf-8'),
    )


def load_tool(name):
    """按文件路径导入 tools/<name>.py（tools 不是包，不能直接 import）。"""
    path = TOOLS / name
    spec = importlib.util.spec_from_file_location('tool_' + path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def point_to(module, root):
    """让自检模块在临时仓库上工作：改掉它的 ROOT 以及由 ROOT 派生的路径。"""
    module.ROOT = root
    if hasattr(module, 'APPS'):
        module.APPS = root / 'main' / 'apps'
    return module


def call_main(module):
    """调用自检模块的 main()，吞掉它的输出，返回 (退出码, 输出)。"""
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        ret = module.main()
    return ret, buf.getvalue()


def write(root, rel, text):
    """在临时仓库里写一个文件（自动建目录）。"""
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
    return path


@contextlib.contextmanager
def temp_repo():
    """一个只含空 main/ 的临时仓库根目录。"""
    with tempfile.TemporaryDirectory(prefix='szpi-tests-') as d:
        root = Path(d)
        (root / 'main').mkdir(parents=True, exist_ok=True)
        yield root
