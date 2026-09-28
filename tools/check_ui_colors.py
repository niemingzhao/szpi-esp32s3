#!/usr/bin/env python3
"""检查 UI 代码里"没有显式设色"的标签。

背景：LVGL 自带主题会给 lv_btn 加"主色底 + 白字"。我们把按钮底色改成卡片色
（bg_card）之后，按钮里的标签如果自己没有设 text_color，就会继承主题给的白字：
深色主题下看不出来，浅色主题下就是"白字白底"，直接看不见
（曾经发生在通知中心的关闭按钮和通用对话框按钮上）。

本脚本扫描 framework/src 与 apps 下的 .c 文件，找出 lv_label_create 之后
若干行内没有 text_color 的地方。改完 UI 跑一下，输出的每一处都应该补上颜色。

用法：
    python tools/check_ui_colors.py
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOOKAHEAD = 6


def scan(path: Path):
    """返回 [(行号, 代码行)]：创建标签但附近没有设置文字颜色。"""
    hits = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if "lv_label_create" not in line:
            continue
        window = lines[i : i + LOOKAHEAD + 1]
        if not any("text_color" in w for w in window):
            hits.append((i + 1, line.strip()))
    return hits


def main() -> int:
    files = sorted((ROOT / "framework" / "src").glob("*.c"))
    files += sorted((ROOT / "apps").rglob("*.c"))

    bad = 0
    for f in files:
        for lineno, text in scan(f):
            if bad == 0:
                print("以下标签没有显式设置文字颜色（浅色主题下可能看不见）：")
            bad += 1
            print(f"  {f.relative_to(ROOT)}:{lineno}: {text}")

    if bad:
        print(f"共 {bad} 处，请补 lv_obj_set_style_text_color(...)。")
        return 1

    print("OK：所有标签都有显式文字颜色")
    return 0


if __name__ == "__main__":
    sys.exit(main())
