"""生成 App 界面通用图标（main/framework/assets/icons_ui.c 与 fw_ui_icons.h）。

和状态栏图标同一套路：20x20，形状画成**白色 + alpha**（RGB565A8），运行时用
image_recolor 染色（行内图标用强调色、置灰状态用 text_disabled），所以资源本身不带颜色，
深浅主题共用一份。

覆盖 App 界面里原来用 LVGL 内置符号（LV_SYMBOL_*）画的那些图标：状态栏返回 / 主页、
行内入口图标、头部动作按钮、月份切换箭头等。

用 4 倍超采样绘制后 LANCZOS 缩小，边缘平滑。RGB565 按小端存放（与显示缓冲同字节序，
写屏回调会再交换一次送给 ST7789）。

用法：
    python tools/gen_ui_icons.py
"""

import math
import os

from PIL import Image, ImageDraw

SS = 4
N = 20
W = N * SS
WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_C = os.path.join(ROOT, "main", "framework", "assets", "icons_ui.c")
ASSET_H = os.path.join(ROOT, "main", "framework", "include", "fw_ui_icons.h")


# ------------------------------- 绘图小工具 -------------------------------

def _box(box):
    return [v * SS for v in box]


def rrect(d, box, radius, fill):
    d.rounded_rectangle(_box(box), radius=radius * SS, fill=fill)


def circle(d, cx, cy, r, fill):
    d.ellipse(_box([cx - r, cy - r, cx + r, cy + r]), fill=fill)


def line(d, pts, width, fill):
    d.line([(x * SS, y * SS) for x, y in pts], fill=fill,
           width=max(1, int(round(width * SS))), joint="curve")


def poly(d, pts, fill):
    d.polygon([(x * SS, y * SS) for x, y in pts], fill=fill)


def arc(d, cx, cy, r, start, end, width, fill):
    d.arc(_box([cx - r, cy - r, cx + r, cy + r]), start, end, fill=fill,
          width=max(1, int(round(width * SS))))


def earc(d, cx, cy, rx, ry, start, end, width, fill):
    d.arc(_box([cx - rx, cy - ry, cx + rx, cy + ry]), start, end, fill=fill,
          width=max(1, int(round(width * SS))))


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


# --------------------------------- 图形 ---------------------------------
# 坐标在 1x（20x20）空间；c = 图形颜色，clr = 挖空用的"透明"

def g_left(d, c, clr):
    line(d, [(12.6, 4.4), (7.0, 10.0), (12.6, 15.6)], 2.0, c)


def g_right(d, c, clr):
    line(d, [(7.4, 4.4), (13.0, 10.0), (7.4, 15.6)], 2.0, c)


def g_left2(d, c, clr):
    line(d, [(10.4, 4.8), (5.0, 10.0), (10.4, 15.2)], 1.8, c)
    line(d, [(16.4, 4.8), (11.0, 10.0), (16.4, 15.2)], 1.8, c)


def g_right2(d, c, clr):
    line(d, [(9.6, 4.8), (15.0, 10.0), (9.6, 15.2)], 1.8, c)
    line(d, [(3.6, 4.8), (9.0, 10.0), (3.6, 15.2)], 1.8, c)


def g_home(d, c, clr):
    line(d, [(2.8, 9.6), (10, 3.2), (17.2, 9.6)], 2.2, c)
    rrect(d, [4.8, 8.8, 15.2, 17.4], 0.8, c)
    rrect(d, [8.3, 12.6, 11.7, 17.4], 0.5, clr)


def g_refresh(d, c, clr):
    arc(d, 10, 10, 5.8, 35, 295, 1.9, c)
    arrow_head(d, 10, 10, 5.8, 300, 3.6, c)


def g_loop(d, c, clr):
    arc(d, 10, 10, 5.8, 205, 335, 1.8, c)
    arrow_head(d, 10, 10, 5.8, 340, 3.2, c)
    arc(d, 10, 10, 5.8, 25, 155, 1.8, c)
    arrow_head(d, 10, 10, 5.8, 160, 3.2, c)


def g_search(d, c, clr):
    circle(d, 8.6, 8.6, 5.0, c)
    circle(d, 8.6, 8.6, 3.2, clr)
    line(d, [(12.4, 12.4), (17.2, 17.2)], 2.0, c)


def g_gear(d, c, clr):
    gear(d, 10, 10, 7.6, 5.6, 8, c)
    circle(d, 10, 10, 2.5, clr)


def g_wifi(d, c, clr):
    circle(d, 10, 15.6, 1.8, c)
    for r, w in ((4.6, 1.5), (8.0, 1.5), (11.4, 1.5)):
        arc(d, 10, 16.4, r, 220, 320, w, c)


def g_bt(d, c, clr):
    line(d, [(10, 3.2), (10, 16.8)], 1.5, c)
    line(d, [(10, 3.2), (14.2, 6.6), (6.4, 12.8)], 1.5, c)
    line(d, [(10, 16.8), (14.2, 13.4), (6.4, 7.2)], 1.5, c)


def g_play(d, c, clr):
    poly(d, [(6.4, 4.0), (15.8, 10.0), (6.4, 16.0)], c)


def g_pause(d, c, clr):
    rrect(d, [5.6, 4.2, 8.8, 15.8], 0.9, c)
    rrect(d, [11.2, 4.2, 14.4, 15.8], 0.9, c)


def g_keyboard(d, c, clr):
    rrect(d, [1.4, 5.2, 18.6, 14.8], 1.8, c)
    rrect(d, [2.9, 6.7, 17.1, 13.3], 1.0, clr)
    for x in (4.2, 7.0, 9.8, 12.6):
        rrect(d, [x, 7.7, x + 2.0, 9.3], 0.3, c)
    rrect(d, [4.8, 10.1, 15.2, 11.9], 0.4, c)


def g_trash(d, c, clr):
    rrect(d, [7.8, 2.2, 12.2, 4.2], 0.7, c)
    rrect(d, [4.2, 4.6, 15.8, 6.8], 0.7, c)
    poly(d, [(6.2, 7.2), (13.8, 7.2), (12.9, 17.6), (7.1, 17.6)], c)
    rrect(d, [8.6, 9.4, 9.8, 15.4], 0.3, clr)
    rrect(d, [10.2, 9.4, 11.4, 15.4], 0.3, clr)


def g_upload(d, c, clr):
    line(d, [(10, 12.6), (10, 3.6)], 2.0, c)
    poly(d, [(10, 2.0), (14.6, 7.0), (5.4, 7.0)], c)
    line(d, [(4.2, 11.8), (4.2, 16.8), (15.8, 16.8), (15.8, 11.8)], 2.0, c)


def g_pin(d, c, clr):
    circle(d, 10, 7.8, 4.8, c)
    poly(d, [(6.4, 10.4), (13.6, 10.4), (10, 18.0)], c)
    circle(d, 10, 7.8, 1.9, clr)


def g_clock(d, c, clr):
    circle(d, 10, 10, 7.6, c)
    circle(d, 10, 10, 6.0, clr)
    line(d, [(10, 5.6), (10, 10.4), (13.8, 12.6)], 1.8, c)


def g_eye(d, c, clr):
    earc(d, 10, 9.8, 8.6, 5.2, 185, 355, 1.8, c)
    earc(d, 10, 9.8, 8.6, 5.2, 5, 175, 1.8, c)
    circle(d, 10, 9.8, 2.3, c)


def g_sun(d, c, clr):
    """亮度"""
    circle(d, 10, 10, 4.4, c)
    for a in range(8):
        ang = math.radians(a * 45)
        x1, y1 = 10 + 6.4 * math.cos(ang), 10 + 6.4 * math.sin(ang)
        x2, y2 = 10 + 8.6 * math.cos(ang), 10 + 8.6 * math.sin(ang)
        line(d, [(x1, y1), (x2, y2)], 1.6, c)


def g_contrast(d, c, clr):
    """主题：左半实心 + 右半描边（明暗对比）"""
    d.pieslice(_box([2.6, 2.6, 17.4, 17.4]), 90, 270, fill=c)
    arc(d, 10, 10, 7.4, -90, 90, 1.7, c)


def g_moon(d, c, clr):
    """熄屏：月牙"""
    circle(d, 10, 10, 7.6, c)
    circle(d, 14.4, 6.6, 6.6, clr)


def g_link(d, c, clr):
    """连接：一段倾斜的空心链节"""
    capsule(d, 5.8, 14.2, 14.2, 5.8, 3.6, c)
    capsule(d, 5.8, 14.2, 14.2, 5.8, 1.9, clr)


def g_lock(d, c, clr):
    """加密 / 需要密码"""
    arc(d, 10, 9.4, 3.7, 180, 360, 1.8, c)          # 锁梁
    rrect(d, [4.6, 9.4, 15.4, 17.8], 1.8, c)        # 锁体
    circle(d, 10, 12.6, 1.2, clr)
    rrect(d, [9.4, 13.4, 10.6, 16.0], 0.3, clr)


def g_mute(d, c, clr):
    poly(d, [(3.4, 7.4), (6.6, 7.4), (10.4, 4.0), (10.4, 16.0), (6.6, 12.6), (3.4, 12.6)], c)
    line(d, [(13.2, 7.6), (17.8, 12.2)], 1.8, c)
    line(d, [(17.8, 7.6), (13.2, 12.2)], 1.8, c)


def g_speaker(d, c, clr):
    poly(d, [(4.4, 7.6), (7.6, 7.6), (11.4, 4.2), (11.4, 15.8), (7.6, 12.4), (4.4, 12.4)], c)
    arc(d, 11, 10, 3.2, -50, 50, 1.4, c)
    arc(d, 11, 10, 5.6, -50, 50, 1.4, c)


def g_hotspot(d, c, clr):
    circle(d, 10, 10, 1.9, c)
    for rx, ry in ((4.0, 4.9), (6.6, 8.1)):
        earc(d, 10, 10, rx, ry, 302, 58, 1.6, c)
        earc(d, 10, 10, rx, ry, 122, 238, 1.6, c)


def g_phone(d, c, clr):
    rrect(d, [5.6, 1.8, 14.4, 18.2], 2.0, c)
    rrect(d, [7.0, 3.8, 13.0, 15.2], 0.8, clr)
    circle(d, 10, 16.8, 0.9, clr)


def g_check(d, c, clr):
    line(d, [(4.6, 10.6), (8.4, 14.4), (15.6, 6.0)], 2.3, c)


def g_close(d, c, clr):
    line(d, [(5.8, 5.8), (14.2, 14.2)], 2.3, c)
    line(d, [(14.2, 5.8), (5.8, 14.2)], 2.3, c)


ICONS = [
    ("left", g_left),
    ("right", g_right),
    ("left2", g_left2),
    ("right2", g_right2),
    ("home", g_home),
    ("refresh", g_refresh),
    ("loop", g_loop),
    ("search", g_search),
    ("gear", g_gear),
    ("wifi", g_wifi),
    ("bt", g_bt),
    ("play", g_play),
    ("pause", g_pause),
    ("keyboard", g_keyboard),
    ("trash", g_trash),
    ("upload", g_upload),
    ("pin", g_pin),
    ("clock", g_clock),
    ("eye", g_eye),
    ("sun", g_sun),
    ("contrast", g_contrast),
    ("moon", g_moon),
    ("link", g_link),
    ("lock", g_lock),
    ("mute", g_mute),
    ("speaker", g_speaker),
    ("hotspot", g_hotspot),
    ("phone", g_phone),
    ("check", g_check),
    ("close", g_close),
]


# --------------------------------- 输出 ---------------------------------

def render(glyph, color=WHITE):
    img = Image.new("RGBA", (W, W), CLEAR)
    glyph(ImageDraw.Draw(img), color, CLEAR)
    return img.resize((N, N), Image.LANCZOS)


def encode_a8(img):
    px = img.load()
    rgb = bytearray()
    a8 = bytearray()
    for y in range(N):
        for x in range(N):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            rgb += bytes((v & 0xFF, v >> 8))
            a8.append(a)
    return bytes(rgb), bytes(a8)


def c_array(name, data):
    lines = []
    for off in range(0, len(data), 16):
        lines.append("    " + " ".join("0x%02X," % b for b in data[off:off + 16]))
    return ("const LV_ATTRIBUTE_MEM_ALIGN uint8_t %s[] = {\n" % name
            + "\n".join(lines) + "\n};\n")


def main():
    parts = [
        "/*\n"
        " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
        " *\n"
        " * App 界面通用图标（自动生成，勿手改）\n"
        " *\n"
        " * 由 tools/gen_ui_icons.py 生成：20x20，形状为白色 + alpha，格式\n"
        " * LV_COLOR_FORMAT_RGB565A8。运行时用 image_recolor 染色（行内图标强调色、\n"
        " * 置灰状态用 text_disabled），深浅主题共用同一份资源。\n"
        " * RGB565 按**小端**存放：LVGL 的图片数据必须与显示缓冲同字节序，写屏回调\n"
        " * periph_lcd_flush_cb() 会再交换一次送给 ST7789。\n"
        " */\n\n"
        "#include \"lvgl.h\"\n\n"
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN\n"
        "#define LV_ATTRIBUTE_MEM_ALIGN\n"
        "#endif\n\n",
    ]

    for name, glyph in ICONS:
        rgb, a8 = encode_a8(render(glyph))
        parts.append(c_array("icon_ui_%s_map" % name, rgb + a8))
        parts.append("\n")
        parts.append("const lv_image_dsc_t icon_ui_%s = {\n" % name)
        parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        parts.append("    .header.cf = LV_COLOR_FORMAT_RGB565A8,\n")
        parts.append("    .header.w = %d,\n" % N)
        parts.append("    .header.h = %d,\n" % N)
        parts.append("    .data_size = %d,\n" % (N * N * 3))
        parts.append("    .data = icon_ui_%s_map,\n" % name)
        parts.append("};\n\n")

    with open(ASSET_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))

    with open(ASSET_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/*\n"
            " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
            " *\n"
            " * Framework - App 界面通用图标\n"
            " *\n"
            " * 自动生成，勿手改，由 tools/gen_ui_icons.py 生成。\n"
            " * 资源是白色 + alpha，运行时用 fw_ui_icon() / fw_ui_icon_btn() /\n"
            " * fw_ui_row_btn_img() 创建时会按主题染色。\n"
            " */\n\n"
            "#pragma once\n\n"
            "#include \"lvgl.h\"\n\n"
        )
        for name, _ in ICONS:
            f.write("LV_IMG_DECLARE(icon_ui_%s);\n" % name)

    print("generated: %s" % ASSET_C)
    print("generated: %s" % ASSET_H)

    # 预览：深色主题（强调色）与浅色主题各一行，另加一行原尺寸放大
    scale = 6
    pad = 6
    bgs = ((0x12, 0x12, 0x12), (0xE7, 0xEC, 0xF2))
    cols = ((0x4F, 0x9E, 0xFF), (0x1D, 0x6F, 0xD0))
    per_row = 12
    rowsn = (len(ICONS) + per_row - 1) // per_row
    sheet_w = pad + per_row * (N * scale + pad)
    sheet = Image.new("RGB", (sheet_w, pad + (rowsn * 2) * (N * scale + pad)), (0x88, 0x88, 0x88))
    for ri, (bg, col) in enumerate(zip(bgs, cols)):
        for k in range(rowsn):
            band = Image.new("RGB", (sheet_w, N * scale + pad), bg)
            for i in range(per_row):
                idx = k * per_row + i
                if idx >= len(ICONS):
                    break
                im = render(ICONS[idx][1], col + (255,)).resize((N * scale, N * scale), Image.NEAREST)
                band.paste(im, (pad + i * (N * scale + pad), pad // 2), im)
            sheet.paste(band, (0, pad // 2 + (ri * rowsn + k) * (N * scale + pad)))
    preview = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons_ui_preview.png")
    sheet.save(preview)
    print("preview: %s" % preview)


if __name__ == "__main__":
    main()
