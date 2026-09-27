#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 App 图标（main/framework/assets/fw_icons.c 与 include/fw_icons.h）。

三组图标内联在同一个 C 文件里（文件尽量合并，不要把图标拆散成多个源文件）：

  icon_ui_*      20x20，白色 + alpha（RGB565A8），运行时用 image_recolor 染色
  icon_home_*    40x40，彩色（圆角渐变底 + 白色图形），桌面 App 图标
  icon_status_*  20x20，白色 + alpha（RGB565A8），状态栏图标

都用 4 倍超采样绘制后 LANCZOS 缩小，边缘平滑。RGB565 一律按小端存放：LVGL 的图片
数据必须与显示缓冲同字节序，写屏回调 periph_lcd_flush_cb() 会再交换一次送给 ST7789。

坐标都在各自尺寸的 1x 空间里（ui / status 是 20x20，home 是 40x40）。

用法：
    python tools/gen_fw_icons.py
"""

import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_C = os.path.join(ROOT, "main", "framework", "assets", "fw_icons.c")
ASSET_H = os.path.join(ROOT, "main", "framework", "include", "fw_icons.h")

WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

# 当前绘制上下文的尺寸与超采样倍数（下面这些绘图函数都用它们）
N = 20
SS = 4
W = N * SS


def set_context(n, ss=4):
    """切换绘制上下文（不同图标的尺寸 / 超采样倍数不一样）。"""
    global N, SS, W
    N, SS, W = n, ss, n * ss


# ------------------------------- 绘图小工具 -------------------------------

def _box(box):
    return [v * SS for v in box]


def rrect(d, box, radius, fill=None, **kw):
    d.rounded_rectangle(_box(box), radius=radius * SS, fill=fill, **kw)


def rect(d, box, **kw):
    d.rectangle(_box(box), **kw)


def circle(d, cx, cy, r, fill=None, **kw):
    d.ellipse(_box([cx - r, cy - r, cx + r, cy + r]), fill=fill, **kw)


def line(d, pts, width, fill=WHITE):
    d.line([(x * SS, y * SS) for x, y in pts], fill=fill,
           width=max(1, int(round(width * SS))), joint="curve")


def poly(d, pts, fill=WHITE):
    d.polygon([(x * SS, y * SS) for x, y in pts], fill=fill)


def arc(d, cx, cy, r, start, end, width, fill=WHITE):
    d.arc(_box([cx - r, cy - r, cx + r, cy + r]), start, end, fill=fill,
          width=max(1, int(round(width * SS))))


def earc(d, cx, cy, rx, ry, start, end, width, fill):
    d.arc(_box([cx - rx, cy - ry, cx + rx, cy + ry]), start, end, fill=fill,
          width=max(1, int(round(width * SS))))


def ellipse_box(d, box, **kw):
    d.ellipse(_box(box), **kw)


def gear(d, cx, cy, r_out, r_in, teeth, fill):
    pts = []
    for i in range(teeth * 2):
        a = math.pi * i / teeth - math.pi / 2
        r = r_out if i % 2 == 0 else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    poly(d, pts, fill)


def arrow_head(d, cx, cy, r, ang_deg, size, fill, cw=True):
    """在圆弧末端沿切线方向画一个箭头（cw = 顺时针方向的切线）。"""
    a = math.radians(ang_deg)
    px, py = cx + r * math.cos(a), cy + r * math.sin(a)
    s = 1.0 if cw else -1.0
    tx, ty = -math.sin(a) * s, math.cos(a) * s
    rx, ry = math.cos(a) * s, math.sin(a) * s
    tip = (px + tx * size, py + ty * size)
    b1 = (px - tx * size * 0.25 + rx * size * 0.8, py - ty * size * 0.25 + ry * size * 0.8)
    b2 = (px - tx * size * 0.25 - rx * size * 0.8, py - ty * size * 0.25 - ry * size * 0.8)
    poly(d, [tip, b1, b2], fill)


def capsule(d, x1, y1, x2, y2, r, fill):
    """两点之间的胶囊形（圆角直线段）。"""
    dx, dy = x2 - x1, y2 - y1
    length = math.hypot(dx, dy)
    if length == 0:
        circle(d, x1, y1, r, fill)
        return
    ux, uy = dx / length, dy / length
    px, py = -uy, ux
    poly(d, [(x1 + px * r, y1 + py * r), (x2 + px * r, y2 + py * r),
             (x2 - px * r, y2 - py * r), (x1 - px * r, y1 - py * r)], fill)
    circle(d, x1, y1, r, fill)
    circle(d, x2, y2, r, fill)


def _cos(deg):
    return math.cos(math.radians(deg))


def _sin(deg):
    return math.sin(math.radians(deg))


# ============================ 1) UI 图标（20x20） ============================
# 坐标在 1x（20x20）空间；c = 图形颜色，clr = 挖空用的"透明"

def ui_g_left(d, c, clr):
    line(d, [(12.6, 4.4), (7.0, 10.0), (12.6, 15.6)], 2.0, c)


def ui_g_right(d, c, clr):
    line(d, [(7.4, 4.4), (13.0, 10.0), (7.4, 15.6)], 2.0, c)


def ui_g_left2(d, c, clr):
    line(d, [(10.4, 4.8), (5.0, 10.0), (10.4, 15.2)], 1.8, c)
    line(d, [(16.4, 4.8), (11.0, 10.0), (16.4, 15.2)], 1.8, c)


def ui_g_right2(d, c, clr):
    line(d, [(9.6, 4.8), (15.0, 10.0), (9.6, 15.2)], 1.8, c)
    line(d, [(3.6, 4.8), (9.0, 10.0), (3.6, 15.2)], 1.8, c)


def ui_g_back(d, c, clr):
    capsule(d, 14.4, 10.0, 5.6, 10.0, 1.05, c)     # 杆
    capsule(d, 10.6, 5.0, 5.6, 10.0, 1.05, c)      # 上臂
    capsule(d, 5.6, 10.0, 10.6, 15.0, 1.05, c)     # 下臂


def ui_g_home(d, c, clr):
    # 外挑屋檐：实心屋顶明显宽于屋身，两端出檐；屋身带门
    poly(d, [(10, 3.0), (17.6, 9.4), (2.4, 9.4)], c)
    rrect(d, [4.8, 8.6, 15.2, 17.2], 0.8, c)
    rrect(d, [8.4, 12.6, 11.6, 17.2], 0.6, clr)


def ui_g_refresh(d, c, clr):
    arc(d, 10, 10, 5.8, 35, 295, 1.9, c)
    arrow_head(d, 10, 10, 5.8, 300, 3.6, c)


def ui_g_loop(d, c, clr):
    arc(d, 10, 10, 5.8, 205, 335, 1.8, c)
    arrow_head(d, 10, 10, 5.8, 340, 3.2, c)
    arc(d, 10, 10, 5.8, 25, 155, 1.8, c)
    arrow_head(d, 10, 10, 5.8, 160, 3.2, c)


def ui_g_search(d, c, clr):
    circle(d, 8.6, 8.6, 5.0, c)
    circle(d, 8.6, 8.6, 3.2, clr)
    line(d, [(12.4, 12.4), (17.2, 17.2)], 2.0, c)


def ui_g_gear(d, c, clr):
    gear(d, 10, 10, 7.6, 5.6, 8, c)
    circle(d, 10, 10, 2.5, clr)


def ui_g_wifi(d, c, clr):
    circle(d, 10, 15.6, 1.8, c)
    for r, w in ((4.6, 1.5), (8.0, 1.5), (11.4, 1.5)):
        arc(d, 10, 16.4, r, 220, 320, w, c)


def ui_g_bt(d, c, clr):
    line(d, [(10, 3.2), (10, 16.8)], 1.5, c)
    line(d, [(10, 3.2), (14.2, 6.6), (6.4, 12.8)], 1.5, c)
    line(d, [(10, 16.8), (14.2, 13.4), (6.4, 7.2)], 1.5, c)


def ui_g_play(d, c, clr):
    poly(d, [(6.4, 4.0), (15.8, 10.0), (6.4, 16.0)], c)


def ui_g_pause(d, c, clr):
    rrect(d, [5.6, 4.2, 8.8, 15.8], 0.9, c)
    rrect(d, [11.2, 4.2, 14.4, 15.8], 0.9, c)


def ui_g_keyboard(d, c, clr):
    rrect(d, [1.4, 5.2, 18.6, 14.8], 1.8, c)
    rrect(d, [2.9, 6.7, 17.1, 13.3], 1.0, clr)
    for x in (4.2, 7.0, 9.8, 12.6):
        rrect(d, [x, 7.7, x + 2.0, 9.3], 0.3, c)
    rrect(d, [4.8, 10.1, 15.2, 11.9], 0.4, c)


def ui_g_trash(d, c, clr):
    rrect(d, [7.8, 2.2, 12.2, 4.2], 0.7, c)
    rrect(d, [4.2, 4.6, 15.8, 6.8], 0.7, c)
    poly(d, [(6.2, 7.2), (13.8, 7.2), (12.9, 17.6), (7.1, 17.6)], c)
    rrect(d, [8.6, 9.4, 9.8, 15.4], 0.3, clr)
    rrect(d, [10.2, 9.4, 11.4, 15.4], 0.3, clr)


def ui_g_upload(d, c, clr):
    line(d, [(10, 12.6), (10, 3.6)], 2.0, c)
    poly(d, [(10, 2.0), (14.6, 7.0), (5.4, 7.0)], c)
    line(d, [(4.2, 11.8), (4.2, 16.8), (15.8, 16.8), (15.8, 11.8)], 2.0, c)


def ui_g_download(d, c, clr):
    """下载：向下的箭头 + 底部横线"""
    line(d, [(10, 2.4), (10, 11.8)], 2.0, c)
    poly(d, [(10, 13.6), (5.4, 8.6), (14.6, 8.6)], c)
    line(d, [(4.4, 16.6), (15.6, 16.6)], 2.0, c)


def ui_g_pin(d, c, clr):
    circle(d, 10, 7.8, 4.8, c)
    poly(d, [(6.4, 10.4), (13.6, 10.4), (10, 18.0)], c)
    circle(d, 10, 7.8, 1.9, clr)


def ui_g_clock(d, c, clr):
    circle(d, 10, 10, 7.6, c)
    circle(d, 10, 10, 6.0, clr)
    line(d, [(10, 5.6), (10, 10.4), (13.8, 12.6)], 1.8, c)


def ui_g_eye(d, c, clr):
    earc(d, 10, 9.8, 8.6, 5.2, 185, 355, 1.8, c)
    earc(d, 10, 9.8, 8.6, 5.2, 5, 175, 1.8, c)
    circle(d, 10, 9.8, 2.3, c)


def ui_g_sun(d, c, clr):
    """亮度"""
    circle(d, 10, 10, 4.4, c)
    for a in range(8):
        ang = math.radians(a * 45)
        x1, y1 = 10 + 6.4 * math.cos(ang), 10 + 6.4 * math.sin(ang)
        x2, y2 = 10 + 8.6 * math.cos(ang), 10 + 8.6 * math.sin(ang)
        line(d, [(x1, y1), (x2, y2)], 1.6, c)


def ui_g_contrast(d, c, clr):
    """主题：左半实心 + 右半描边（明暗对比）"""
    d.pieslice(_box([2.6, 2.6, 17.4, 17.4]), 90, 270, fill=c)
    arc(d, 10, 10, 7.4, -90, 90, 1.7, c)


def ui_g_moon(d, c, clr):
    """熄屏：月牙"""
    circle(d, 10, 10, 7.6, c)
    circle(d, 14.4, 6.6, 6.6, clr)


def ui_g_cloud(d, c, clr):
    """阴 / 多云：一朵云"""
    circle(d, 6.9, 11.3, 3.1, c)
    circle(d, 10.5, 9.7, 4.0, c)
    circle(d, 13.9, 11.4, 2.8, c)
    rrect(d, [4.2, 10.9, 16.4, 14.0], 1.55, c)


def ui_g_rain(d, c, clr):
    """雨：云 + 三条雨丝"""
    circle(d, 6.8, 8.8, 2.8, c)
    circle(d, 10.2, 7.5, 3.6, c)
    circle(d, 13.4, 9.0, 2.6, c)
    rrect(d, [4.2, 8.6, 15.9, 11.4], 1.45, c)
    line(d, [(7.0, 13.1), (6.2, 16.5)], 1.5, c)
    line(d, [(10.0, 13.1), (9.2, 16.5)], 1.5, c)
    line(d, [(13.0, 13.1), (12.2, 16.5)], 1.5, c)


def ui_g_snow(d, c, clr):
    """雪：云 + 三个雪点"""
    circle(d, 6.8, 8.8, 2.8, c)
    circle(d, 10.2, 7.5, 3.6, c)
    circle(d, 13.4, 9.0, 2.6, c)
    rrect(d, [4.2, 8.6, 15.9, 11.4], 1.45, c)
    circle(d, 6.8, 14.5, 1.15, c)
    circle(d, 10.0, 16.3, 1.15, c)
    circle(d, 13.2, 14.5, 1.15, c)


def ui_g_fog(d, c, clr):
    """雾 / 霾：云 + 三条雾线"""
    circle(d, 6.9, 7.7, 2.7, c)
    circle(d, 10.4, 6.5, 3.5, c)
    circle(d, 13.6, 7.9, 2.5, c)
    rrect(d, [4.4, 7.5, 16.0, 10.1], 1.4, c)
    line(d, [(3.8, 12.9), (16.2, 12.9)], 1.5, c)
    line(d, [(5.8, 15.9), (14.2, 15.9)], 1.5, c)
    line(d, [(4.4, 18.5), (13.0, 18.5)], 1.4, c)


def ui_g_thunder(d, c, clr):
    """雷：云 + 闪电"""
    circle(d, 6.8, 8.8, 2.8, c)
    circle(d, 10.2, 7.5, 3.6, c)
    circle(d, 13.4, 9.0, 2.6, c)
    rrect(d, [4.2, 8.6, 15.9, 11.4], 1.45, c)
    poly(d, [(11.4, 12.2), (7.6, 16.6), (10.0, 16.6), (9.2, 19.4),
             (13.0, 14.6), (10.5, 14.6)], c)


def ui_g_link(d, c, clr):
    """连接：一段倾斜的空心链节"""
    capsule(d, 5.8, 14.2, 14.2, 5.8, 3.6, c)
    capsule(d, 5.8, 14.2, 14.2, 5.8, 1.9, clr)


def ui_g_lock(d, c, clr):
    """加密 / 需要密码"""
    arc(d, 10, 9.4, 3.7, 180, 360, 1.8, c)          # 锁梁
    rrect(d, [4.6, 9.4, 15.4, 17.8], 1.8, c)        # 锁体
    circle(d, 10, 12.6, 1.2, clr)
    rrect(d, [9.4, 13.4, 10.6, 16.0], 0.3, clr)


def ui_g_mute(d, c, clr):
    poly(d, [(3.4, 7.4), (6.6, 7.4), (10.4, 4.0), (10.4, 16.0), (6.6, 12.6), (3.4, 12.6)], c)
    line(d, [(13.2, 7.6), (17.8, 12.2)], 1.8, c)
    line(d, [(17.8, 7.6), (13.2, 12.2)], 1.8, c)


def ui_g_speaker(d, c, clr):
    poly(d, [(4.4, 7.6), (7.6, 7.6), (11.4, 4.2), (11.4, 15.8), (7.6, 12.4), (4.4, 12.4)], c)
    arc(d, 11, 10, 3.2, -50, 50, 1.4, c)
    arc(d, 11, 10, 5.6, -50, 50, 1.4, c)


def ui_g_mic(d, c, clr):
    """录音：话筒 + 话筒架"""
    rrect(d, [7.9, 1.9, 12.1, 11.1], 2.1, c)
    arc(d, 10, 10.2, 5.0, 0, 180, 1.7, c)               # 下半圆弧的话筒架
    line(d, [(10, 15.2), (10, 17.5)], 1.7, c)
    line(d, [(6.9, 17.5), (13.1, 17.5)], 1.7, c)
    circle(d, 10, 6.4, 1.1, clr)                        # 话筒头上的收音孔


def ui_g_stop(d, c, clr):
    """停止：圆角方块"""
    rrect(d, [5.4, 5.4, 14.6, 14.6], 1.9, c)


def ui_g_flag(d, c, clr):
    """计次：小旗子"""
    line(d, [(5.4, 2.8), (5.4, 17.6)], 2.0, c)
    poly(d, [(6.8, 3.4), (15.6, 6.6), (6.8, 9.8)], c)


def ui_g_warning(d, c, clr):
    """告警：三角 + 感叹号（感叹号用底色挖出来）"""
    poly(d, [(10, 2.6), (18.4, 17.4), (1.6, 17.4)], c)
    rrect(d, [9.2, 7.6, 10.8, 12.8], 0.7, clr)
    circle(d, 10, 14.9, 0.9, clr)


def ui_g_filter(d, c, clr):
    """筛选：漏斗"""
    poly(d, [(2.8, 4.0), (17.2, 4.0), (11.8, 10.6), (11.8, 17.6), (8.2, 15.0), (8.2, 10.6)], c)


def ui_g_hotspot(d, c, clr):
    circle(d, 10, 10, 1.9, c)
    for rx, ry in ((4.0, 4.9), (6.6, 8.1)):
        earc(d, 10, 10, rx, ry, 302, 58, 1.6, c)
        earc(d, 10, 10, rx, ry, 122, 238, 1.6, c)


def ui_g_phone(d, c, clr):
    rrect(d, [5.6, 1.8, 14.4, 18.2], 2.0, c)
    rrect(d, [7.0, 3.8, 13.0, 15.2], 0.8, clr)
    circle(d, 10, 16.8, 0.9, clr)


def ui_g_check(d, c, clr):
    line(d, [(4.6, 10.6), (8.4, 14.4), (15.6, 6.0)], 2.3, c)


def ui_g_close(d, c, clr):
    line(d, [(5.8, 5.8), (14.2, 14.2)], 2.3, c)
    line(d, [(14.2, 5.8), (5.8, 14.2)], 2.3, c)


def ui_g_up(d, c, clr):
    """上一级：向上箭头"""
    line(d, [(10, 17.0), (10, 5.6)], 2.0, c)
    poly(d, [(10, 3.0), (15.2, 8.6), (4.8, 8.6)], c)


def ui_g_more(d, c, clr):
    """更多：三个点"""
    for x in (4.4, 10.0, 15.6):
        circle(d, x, 10, 1.7, c)


def _doc_body(d, c, clr):
    """文件的公共外形：一张右上角折角的纸"""
    poly(d, [(5.0, 2.6), (12.2, 2.6), (15.2, 5.8), (15.2, 17.4), (5.0, 17.4)], c)
    poly(d, [(12.2, 2.6), (15.2, 5.8), (12.2, 5.8)], clr)


def ui_g_file(d, c, clr):
    """通用文件"""
    _doc_body(d, c, clr)


def ui_g_file_text(d, c, clr):
    """文本文件：纸 + 三条文本线"""
    _doc_body(d, c, clr)
    for y in (9.0, 11.8, 14.6):
        rrect(d, [7.2, y, 13.4, y + 1.1], 0.4, clr)


def ui_g_file_audio(d, c, clr):
    """音频文件：纸 + 一个音符"""
    _doc_body(d, c, clr)
    circle(d, 8.6, 14.8, 1.9, clr)
    rrect(d, [10.0, 9.4, 11.2, 15.0], 0.3, clr)
    poly(d, [(11.2, 9.4), (13.8, 10.2), (13.8, 12.2), (11.2, 11.4)], clr)


def ui_g_file_image(d, c, clr):
    """图片文件：纸 + 山与太阳"""
    _doc_body(d, c, clr)
    poly(d, [(6.6, 15.8), (9.8, 11.0), (13.0, 15.8)], clr)
    circle(d, 11.4, 9.0, 1.3, clr)


def ui_g_zoom_in(d, c, clr):
    """放大：放大镜 + 加号"""
    circle(d, 8.4, 8.4, 5.6, c)                     # 镜框
    circle(d, 8.4, 8.4, 3.9, clr)                   # 镜片（挖空）
    line(d, [(12.6, 12.6), (17.6, 17.6)], 2.2, c)   # 手柄
    rrect(d, [5.3, 7.8, 11.5, 9.0], 0.3, c)         # 横
    rrect(d, [7.8, 5.3, 9.0, 11.5], 0.3, c)         # 竖


def ui_g_zoom_out(d, c, clr):
    """缩小：放大镜 + 减号"""
    circle(d, 8.4, 8.4, 5.6, c)
    circle(d, 8.4, 8.4, 3.9, clr)
    line(d, [(12.6, 12.6), (17.6, 17.6)], 2.2, c)
    rrect(d, [5.3, 7.8, 11.5, 9.0], 0.3, c)


def ui_g_camera(d, c, clr):
    """拍照：机身 + 顶部取景凸起 + 镜头"""
    rrect(d, [2.6, 6.2, 17.4, 16.8], 2.0, c)        # 机身
    rrect(d, [6.8, 3.4, 13.2, 6.6], 0.8, c)         # 取景凸起
    circle(d, 10, 11.6, 3.6, clr)                   # 镜头外圈（挖空）
    circle(d, 10, 11.6, 1.9, c)                     # 镜头内芯


def ui_g_save(d, c, clr):
    """保存：软盘"""
    rrect(d, [3.6, 3.4, 16.4, 16.6], 1.6, c)        # 盘体
    rrect(d, [6.8, 4.8, 13.2, 8.8], 0.4, clr)       # 上部标签
    rrect(d, [6.6, 11.4, 13.4, 16.2], 0.6, clr)     # 下部金属滑盖
    rrect(d, [8.4, 12.6, 11.6, 15.0], 0.3, c)       # 滑盖上的推钮


def ui_g_folder(d, c, clr):
    """文件夹：标签页 + 主体"""
    rrect(d, [2.6, 2.8, 9.2, 6.8], 1.0, c)
    rrect(d, [2.6, 4.8, 17.4, 16.0], 2.0, c)


def ui_g_folder_plus(d, c, clr):
    """新建文件夹：文件夹中间的加号"""
    rrect(d, [2.6, 2.8, 9.2, 6.8], 1.0, c)
    rrect(d, [2.6, 4.8, 17.4, 16.0], 2.0, c)
    rrect(d, [7.2, 10.3, 12.8, 11.7], 0.3, clr)
    rrect(d, [9.3, 8.2, 10.7, 13.8], 0.3, clr)


UI_ICONS = [
    ("left", ui_g_left),
    ("right", ui_g_right),
    ("left2", ui_g_left2),
    ("right2", ui_g_right2),
    ("back", ui_g_back),
    ("home", ui_g_home),
    ("refresh", ui_g_refresh),
    ("loop", ui_g_loop),
    ("search", ui_g_search),
    ("gear", ui_g_gear),
    ("wifi", ui_g_wifi),
    ("bt", ui_g_bt),
    ("play", ui_g_play),
    ("pause", ui_g_pause),
    ("keyboard", ui_g_keyboard),
    ("trash", ui_g_trash),
    ("upload", ui_g_upload),
    ("download", ui_g_download),
    ("pin", ui_g_pin),
    ("clock", ui_g_clock),
    ("eye", ui_g_eye),
    ("sun", ui_g_sun),
    ("contrast", ui_g_contrast),
    ("moon", ui_g_moon),
    ("cloud", ui_g_cloud),
    ("rain", ui_g_rain),
    ("snow", ui_g_snow),
    ("fog", ui_g_fog),
    ("thunder", ui_g_thunder),
    ("up", ui_g_up),
    ("more", ui_g_more),
    ("file", ui_g_file),
    ("file_text", ui_g_file_text),
    ("file_audio", ui_g_file_audio),
    ("file_image", ui_g_file_image),
    ("camera", ui_g_camera),
    ("zoom_in", ui_g_zoom_in),
    ("zoom_out", ui_g_zoom_out),
    ("save", ui_g_save),
    ("folder", ui_g_folder),
    ("folder_plus", ui_g_folder_plus),
    ("link", ui_g_link),
    ("lock", ui_g_lock),
    ("mute", ui_g_mute),
    ("speaker", ui_g_speaker),
    ("mic", ui_g_mic),
    ("stop", ui_g_stop),
    ("flag", ui_g_flag),
    ("warning", ui_g_warning),
    ("filter", ui_g_filter),
    ("hotspot", ui_g_hotspot),
    ("phone", ui_g_phone),
    ("check", ui_g_check),
    ("close", ui_g_close),
]


# ========================== 2) 桌面图标（40x40 彩色） ==========================
# 坐标在 1x（40x40）空间；bg 用于"挖空"（把图形画成底色）

def home_g_scripts(d, bg):
    line(d, [(16, 13), (10, 20), (16, 27)], 2.6)
    line(d, [(24, 13), (30, 20), (24, 27)], 2.6)
    line(d, [(23.5, 12), (17, 28)], 2.6)


def home_g_clock(d, bg):
    circle(d, 20, 21, 11, outline=WHITE, width=int(2.4 * SS))
    line(d, [(20, 21), (20, 14.5)], 2.2)
    line(d, [(20, 21), (25.5, 22.5)], 2.2)
    circle(d, 20, 21, 1.7, fill=WHITE)


def home_g_calendar(d, bg):
    rrect(d, [9, 11, 31, 31], 3, outline=WHITE, width=int(2.3 * SS))
    rect(d, [10.6, 12.6, 29.4, 17.6], fill=WHITE)
    line(d, [(14, 8), (14, 13)], 2.3)
    line(d, [(26, 8), (26, 13)], 2.3)
    circle(d, 14.5, 22, 1.6, fill=WHITE)
    circle(d, 20, 22, 1.6, fill=WHITE)
    circle(d, 25.5, 22, 1.6, fill=WHITE)
    circle(d, 14.5, 26.5, 1.6, fill=WHITE)
    circle(d, 20, 26.5, 1.6, fill=WHITE)


def home_g_weather(d, bg):
    circle(d, 15.5, 15, 6.2, fill=WHITE)
    for a in range(0, 360, 45):
        line(d, [(15.5 + 8.6 * _cos(a), 15 + 8.6 * _sin(a)),
                 (15.5 + 11.4 * _cos(a), 15 + 11.4 * _sin(a))], 2.0)
    circle(d, 20, 24.5, 6.4, fill=WHITE)
    circle(d, 26.5, 25.5, 5.0, fill=WHITE)
    rrect(d, [13.5, 24, 31.5, 31], 3.4, fill=WHITE)


def home_g_wifi(d, bg):
    circle(d, 20, 29.5, 2.3, fill=WHITE)
    arc(d, 20, 30.5, 7, 218, 322, 2.5)
    arc(d, 20, 30.5, 12, 218, 322, 2.5)
    arc(d, 20, 30.5, 17, 218, 322, 2.5)


def home_g_bt(d, bg):
    line(d, [(20, 8.5), (20, 31)], 2.5)
    line(d, [(20, 8.5), (27, 14), (13.5, 24.5)], 2.5)
    line(d, [(20, 31), (27, 25.5), (13.5, 15.5)], 2.5)


def home_g_display(d, bg):
    rrect(d, [7, 10, 33, 26], 3, outline=WHITE, width=int(2.4 * SS))
    poly(d, [(17.5, 26), (22.5, 26), (24, 30), (16, 30)])
    rrect(d, [12, 30, 28, 32.5], 1.2, fill=WHITE)


def home_g_sound(d, bg):
    poly(d, [(11, 16.5), (16.5, 16.5), (23, 10.5), (23, 29.5), (16.5, 23.5), (11, 23.5)])
    arc(d, 22, 20, 6.0, -52, 52, 2.3)
    arc(d, 22, 20, 10.0, -52, 52, 2.3)


def home_g_file(d, bg):
    poly(d, [(8, 11), (16.5, 11), (19, 14.5), (32, 14.5), (32, 30), (8, 30)])
    rect(d, [11, 18, 29, 30], fill=(0, 0, 0, 0))


def home_g_editor(d, bg):
    rrect(d, [11, 8, 29, 32], 2.5, outline=WHITE, width=int(2.3 * SS))
    line(d, [(15, 15), (25, 15)], 2.0)
    line(d, [(15, 20), (25, 20)], 2.0)
    line(d, [(15, 25), (21, 25)], 2.0)


def home_g_calc(d, bg):
    rrect(d, [10, 8, 30, 32], 3, outline=WHITE, width=int(2.4 * SS))
    rrect(d, [13.5, 11.5, 26.5, 15.5], 1.4, fill=WHITE)
    for x in (15, 20, 25):
        for y in (20, 25.5):
            circle(d, x, y, 1.7, fill=WHITE)


def home_g_download(d, bg):
    line(d, [(20, 9), (20, 23)], 2.7)
    poly(d, [(14.5, 20.5), (25.5, 20.5), (20, 27.5)])
    line(d, [(11, 26.5), (11, 31), (29, 31), (29, 26.5)], 2.7)


def home_g_music(d, bg):
    ellipse_box(d, [11.5, 24.5, 19.5, 31], fill=WHITE)
    line(d, [(18.5, 27.5), (18.5, 10.5)], 2.4)
    poly(d, [(18.5, 9.5), (29.5, 13), (29.5, 18.5), (18.5, 15)])


def home_g_recorder(d, bg):
    rrect(d, [16, 8, 24, 21], 4, fill=WHITE)
    arc(d, 20, 20, 8.5, 0, 180, 2.4)
    line(d, [(20, 28.5), (20, 32)], 2.4)
    line(d, [(16, 32), (24, 32)], 2.4)


def home_g_camera(d, bg):
    rrect(d, [7, 12, 33, 30], 3, fill=WHITE)
    rrect(d, [14, 8.5, 22, 12.5], 1.5, fill=WHITE)
    circle(d, 20, 21, 4.8, fill=bg)
    circle(d, 20, 21, 2.0, fill=WHITE)


def home_g_image(d, bg):
    rrect(d, [8, 10, 32, 30], 2.5, outline=WHITE, width=int(2.4 * SS))
    poly(d, [(12, 26), (18, 18), (22.5, 23), (26, 18.5), (30, 26)])
    circle(d, 15, 15, 2.2, fill=WHITE)


def home_g_stopwatch(d, bg):
    circle(d, 20, 22, 10.5, outline=WHITE, width=int(2.4 * SS))
    rrect(d, [17.5, 6.5, 22.5, 9.5], 1.2, fill=WHITE)
    line(d, [(26, 11.5), (29, 8.5)], 2.2)
    line(d, [(20, 22), (20, 15.5)], 2.2)
    circle(d, 20, 22, 1.7, fill=WHITE)


def home_g_timer(d, bg):
    rrect(d, [12, 7.5, 28, 11], 1.6, fill=WHITE)
    rrect(d, [12, 29, 28, 32.5], 1.6, fill=WHITE)
    poly(d, [(14, 12), (26, 12), (20, 20)])
    poly(d, [(14, 28), (26, 28), (20, 20)])


def home_g_imu(d, bg):
    circle(d, 20, 20.5, 3.0, fill=WHITE)
    line(d, [(20, 20.5), (30, 20.5)], 2.3)
    line(d, [(20, 20.5), (12.5, 12.5)], 2.3)
    line(d, [(20, 20.5), (20, 31)], 2.3)
    poly(d, [(30, 20.5), (25.5, 17.8), (25.5, 23.2)])
    poly(d, [(12.5, 12.5), (14.6, 17.6), (18.4, 14.2)])
    poly(d, [(20, 31), (17.3, 26.6), (22.7, 26.6)])


def home_g_perf(d, bg):
    arc(d, 20, 23, 11.5, 180, 360, 2.6)
    line(d, [(20, 23), (26.5, 15.5)], 2.3)
    circle(d, 20, 23, 2.0, fill=WHITE)
    line(d, [(11, 23), (11, 26)], 2.0)
    line(d, [(29, 23), (29, 26)], 2.0)


def home_g_log(d, bg):
    rrect(d, [10, 9, 30, 31], 2.5, outline=WHITE, width=int(2.3 * SS))
    for y in (15, 20, 25):
        circle(d, 14, y, 1.3, fill=WHITE)
        line(d, [(17.5, y), (26, y)], 2.0)


def home_g_about(d, bg):
    circle(d, 20, 20.5, 11.5, outline=WHITE, width=int(2.4 * SS))
    circle(d, 20, 14, 1.9, fill=WHITE)
    rrect(d, [18.8, 17.5, 21.2, 26.5], 1.2, fill=WHITE)


# 顺序 = 原始需求「内置应用」列表的顺序；(名称, 渐变顶色, 渐变底色, 图形)
HOME_ICONS = [
    ("scripts",   (0x4E, 0x5D, 0x78), (0x39, 0x45, 0x5C), home_g_scripts),
    ("clock",     (0x4F, 0x9E, 0xFF), (0x2D, 0x6C, 0xD8), home_g_clock),
    ("calendar",  (0xFF, 0x70, 0x70), (0xE0, 0x3E, 0x3E), home_g_calendar),
    ("weather",   (0x5B, 0xC8, 0xF5), (0x2D, 0x9C, 0xDB), home_g_weather),
    ("wifi",      (0x3D, 0xC1, 0x8B), (0x1E, 0x9E, 0x6E), home_g_wifi),
    ("bt",        (0x6C, 0x74, 0xF0), (0x4A, 0x50, 0xD0), home_g_bt),
    ("display",   (0xA5, 0x6B, 0xF0), (0x7E, 0x3F, 0xD8), home_g_display),
    ("sound",     (0xFF, 0x9A, 0x4D), (0xEF, 0x6F, 0x24), home_g_sound),
    ("file",      (0xFF, 0xC1, 0x4D), (0xF0, 0x9A, 0x18), home_g_file),
    ("editor",    (0x4D, 0xB0, 0xE0), (0x25, 0x82, 0xB8), home_g_editor),
    ("calc",      (0x2F, 0xC4, 0xB0), (0x14, 0x9E, 0x8C), home_g_calc),
    ("download",  (0x59, 0xC4, 0xF5), (0x2A, 0x93, 0xDB), home_g_download),
    ("music",     (0xEE, 0x6C, 0xA8), (0xD1, 0x3D, 0x84), home_g_music),
    ("recorder",  (0xF0, 0x5A, 0x54), (0xCC, 0x33, 0x33), home_g_recorder),
    ("camera",    (0x46, 0x55, 0x69), (0x2B, 0x35, 0x45), home_g_camera),
    ("image",     (0x8B, 0x74, 0xFF), (0x64, 0x48, 0xE0), home_g_image),
    ("stopwatch", (0xFF, 0xA7, 0x4D), (0xEE, 0x7E, 0x1E), home_g_stopwatch),
    ("timer",     (0xC6, 0x8B, 0x5A), (0xA0, 0x67, 0x38), home_g_timer),
    ("imu",       (0x74, 0x86, 0x9B), (0x4E, 0x5D, 0x70), home_g_imu),
    ("perf",      (0x3D, 0xC1, 0x8B), (0x1E, 0x9E, 0x6E), home_g_perf),
    ("log",       (0x8E, 0x9A, 0xA8), (0x66, 0x74, 0x84), home_g_log),
    ("about",     (0x4F, 0x9E, 0xFF), (0x2D, 0x6C, 0xD8), home_g_about),
]


# ========================= 3) 状态栏图标（20x20） =========================
# 坐标在 1x（20x20）空间；c = 图形颜色，clear = 挖空用的"透明"

def status_g_wifi(d, c, clear):
    circle(d, 10, 15.6, 1.8, c)
    for r, w in ((4.6, 1.5), (8.0, 1.5), (11.4, 1.5)):
        arc(d, 10, 16.4, r, 220, 320, w, c)


def status_g_bt(d, c, clear):
    line(d, [(10, 3.2), (10, 16.8)], 1.5, c)
    line(d, [(10, 3.2), (14.2, 6.6), (6.4, 12.8)], 1.5, c)
    line(d, [(10, 16.8), (14.2, 13.4), (6.4, 7.2)], 1.5, c)


def status_g_sound(d, c, clear):
    poly(d, [(4.4, 7.6), (7.6, 7.6), (11.4, 4.2), (11.4, 15.8), (7.6, 12.4), (4.4, 12.4)], c)
    arc(d, 11, 10, 3.2, -50, 50, 1.4, c)
    arc(d, 11, 10, 5.6, -50, 50, 1.4, c)


def status_g_record(d, c, clear):
    rrect(d, [7.4, 2.4, 12.6, 11.6], 2.6, c)
    arc(d, 10, 10.6, 4.8, 0, 180, 1.5, c)
    line(d, [(10, 15.4), (10, 17.6)], 1.5, c)
    line(d, [(6.8, 17.6), (13.2, 17.6)], 1.5, c)


def status_g_camera(d, c, clear):
    rrect(d, [1.4, 6.0, 18.6, 16.8], 2.2, c)
    rrect(d, [5.8, 3.4, 11.4, 6.4], 0.9, c)
    circle(d, 10, 11.4, 4.2, clear)        # 镜头外圈挖空
    circle(d, 10, 11.4, 1.7, c)            # 镜心


def status_g_sd(d, c, clear):
    poly(d, [(4.2, 2.4), (12.6, 2.4), (15.8, 5.6), (15.8, 17.6), (4.2, 17.6)], c)
    rrect(d, [6.6, 4.6, 8.2, 8.0], 0.3, clear)     # 触点槽
    rrect(d, [9.2, 4.6, 10.8, 8.0], 0.3, clear)
    rrect(d, [11.8, 5.6, 13.4, 9.0], 0.3, clear)


STATUS_ICONS = [
    ("wifi", status_g_wifi),
    ("bt", status_g_bt),
    ("sound", status_g_sound),
    ("record", status_g_record),
    ("camera", status_g_camera),
    ("sd", status_g_sd),
]


# --------------------------------- 输出 ---------------------------------

def render_plain(glyph, color=WHITE):
    """白色（或指定色）+ alpha，透明底；ui / status 图标用。"""
    img = Image.new("RGBA", (W, W), CLEAR)
    glyph(ImageDraw.Draw(img), color, CLEAR)
    return img.resize((N, N), Image.LANCZOS)


def render_home(bg_top, bg_bottom, glyph):
    """桌面图标：竖向渐变圆角底 + 白色图形，返回 NxN RGBA。"""
    grad = Image.new("RGB", (W, W))
    gd = ImageDraw.Draw(grad)
    for y in range(W):
        t = y / (W - 1)
        col = tuple(int(bg_top[i] + (bg_bottom[i] - bg_top[i]) * t) for i in range(3))
        gd.line([(0, y), (W, y)], fill=col)

    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([2 * SS, 2 * SS, 38 * SS, 38 * SS],
                                           radius=10 * SS, fill=255)

    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)

    layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    glyph(ImageDraw.Draw(layer), bg_bottom + (255,))
    return Image.alpha_composite(img, layer).resize((N, N), Image.LANCZOS)


def encode_a8(img):
    """返回 (rgb565_le_bytes, a8_bytes)"""
    px = img.load()
    rgb = bytearray()
    a8 = bytearray()
    for y in range(N):
        for x in range(N):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb += bytes((v & 0xFF, v >> 8))    # 小端
            a8.append(a)
    return bytes(rgb), bytes(a8)


def c_array(name, data):
    lines = []
    for off in range(0, len(data), 16):
        lines.append("    " + " ".join("0x%02X," % b for b in data[off:off + 16]))
    return ("const LV_ATTRIBUTE_MEM_ALIGN uint8_t %s[] = {\n" % name
            + "\n".join(lines) + "\n};\n")


def emit_icon(parts, full_name, img):
    """追加一个图标的位图数组与 image dsc。"""
    rgb, a8 = encode_a8(img)
    parts.append(c_array("%s_map" % full_name, rgb + a8))
    parts.append("\n")
    parts.append("const lv_image_dsc_t %s = {\n" % full_name)
    parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
    parts.append("    .header.cf = LV_COLOR_FORMAT_RGB565A8,\n")
    parts.append("    .header.w = %d,\n" % N)
    parts.append("    .header.h = %d,\n" % N)
    parts.append("    .data_size = %d,\n" % (N * N * 3))
    parts.append("    .data = %s_map,\n" % full_name)
    parts.append("};\n\n")


def preview_sheet(path, images, rows, colors, scale, pad=6):
    """预览图：每行一种背景 + 染色，便于人工确认造型。"""
    cell = images[0].width * scale
    sheet = Image.new("RGB", (pad + len(images) * (cell + pad), pad + len(rows) * (cell + pad)),
                      (0x88, 0x88, 0x88))
    for ri, (bg, col) in enumerate(zip(rows, colors)):
        band = Image.new("RGB", (len(images) * (cell + pad) + pad, cell + pad), bg)
        for i, im in enumerate(images):
            tile = im.resize((cell, cell), Image.NEAREST)
            band.paste(tile, (pad + i * (cell + pad), pad // 2), tile)
        sheet.paste(band, (0, pad // 2 + ri * (cell + pad)))
    sheet.save(path)
    print("preview: %s" % path)


def main():
    parts = [
        "/*\n"
        " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
        " *\n"
        " * App 图标（自动生成，勿手改）\n"
        " *\n"
        " * 由 tools/gen_fw_icons.py 生成，三组：\n"
        " *   icon_ui_*      20x20，白色 + alpha，运行时 image_recolor 染色\n"
        " *   icon_home_*    40x40，彩色（圆角渐变底 + 白色图形），桌面 App 图标\n"
        " *   icon_status_*  20x20，白色 + alpha，状态栏图标\n"
        " *\n"
        " * 格式都是 LV_COLOR_FORMAT_RGB565A8。RGB565 按小端存放：LVGL 的图片数据\n"
        " * 必须与显示缓冲同字节序，写屏回调 periph_lcd_flush_cb() 会再交换一次送给 ST7789。\n"
        " */\n\n"
        "#include \"lvgl.h\"\n"
        "#include \"fw_icons.h\"\n\n"
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN\n"
        "#define LV_ATTRIBUTE_MEM_ALIGN\n"
        "#endif\n\n",
    ]

    # 1) UI 图标
    set_context(20)
    parts.append("/* ------------------------------ icon_ui_*（20x20 单色） ------------------------------ */\n\n")
    ui_images = []
    for name, glyph in UI_ICONS:
        img = render_plain(glyph)
        emit_icon(parts, "icon_ui_%s" % name, img)
        ui_images.append(render_plain(glyph, (0x4F, 0x9E, 0xFF, 255)))

    # 2) 桌面图标
    set_context(40)
    parts.append("/* ----------------------------- icon_home_*（40x40 彩色） ----------------------------- */\n\n")
    home_images = []
    for name, top, bottom, glyph in HOME_ICONS:
        img = render_home(top, bottom, glyph)
        emit_icon(parts, "icon_home_%s" % name, img)
        home_images.append(img)

    # 3) 状态栏图标
    set_context(20)
    parts.append("/* ---------------------------- icon_status_*（20x20 单色） ---------------------------- */\n\n")
    status_images = []
    for name, glyph in STATUS_ICONS:
        img = render_plain(glyph)
        emit_icon(parts, "icon_status_%s" % name, img)
        status_images.append(render_plain(glyph, (0x4F, 0x9E, 0xFF, 255)))

    with open(ASSET_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))

    header = (
        "/*\n"
        " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
        " *\n"
        " * App 图标声明（实现由 tools/gen_fw_icons.py 生成到 assets/fw_icons.c）\n"
        " *\n"
        " * icon_ui_*      20x20，白色 + alpha：界面功能图标，运行时按主题 / 用途染色\n"
        " *                （fw_ui_icon() / fw_ui_icon_btn() / fw_ui_row_btn_img()）\n"
        " * icon_home_*    40x40，彩色：桌面 App 图标（fw_app_desc_t.icon）\n"
        " * icon_status_*  20x20，白色 + alpha：状态栏状态图标\n"
        " */\n\n"
        "#pragma once\n\n"
        "#include \"lvgl.h\"\n\n"
        "#ifdef __cplusplus\n"
        "extern \"C\" {\n"
        "#endif\n\n"
    )
    for name, _ in UI_ICONS:
        header += "LV_IMG_DECLARE(icon_ui_%s);\n" % name
    header += "\n"
    for name, _, _, _ in HOME_ICONS:
        header += "LV_IMG_DECLARE(icon_home_%s);\n" % name
    header += "\n"
    for name, _ in STATUS_ICONS:
        header += "LV_IMG_DECLARE(icon_status_%s);\n" % name
    header += "\n#ifdef __cplusplus\n}\n#endif\n"

    with open(ASSET_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)

    print("generated: %s" % ASSET_C)
    print("generated: %s" % ASSET_H)
    print("icons: ui=%d home=%d status=%d" % (len(UI_ICONS), len(HOME_ICONS), len(STATUS_ICONS)))

    here = os.path.dirname(os.path.abspath(__file__))
    preview_sheet(os.path.join(here, "icons_ui_preview.png"), ui_images,
                  ((0x12, 0x12, 0x12), (0xE7, 0xEC, 0xF2)),
                  ((0x4F, 0x9E, 0xFF, 255), (0x1D, 0x6F, 0xD0, 255)), 6)
    preview_sheet(os.path.join(here, "icons_status_preview.png"), status_images,
                  ((0x1E, 0x1E, 0x1E), (0xFF, 0xFF, 0xFF)),
                  ((0x4F, 0x9E, 0xFF, 255), (0x1D, 0x6F, 0xD0, 255)), 8)

    sheet = Image.new("RGB", (len(home_images) * (40 + 6) + 6, 40 + 12), (32, 32, 32))
    for i, im in enumerate(home_images):
        sheet.paste(im, (6 + i * (40 + 6), 6), im)
    sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
    preview = os.path.join(here, "icons_home_preview.png")
    sheet.save(preview)
    print("preview: %s" % preview)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
