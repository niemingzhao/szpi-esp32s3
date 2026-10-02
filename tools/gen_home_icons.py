"""生成桌面 App 的彩色图标（main/framework/assets/icons_home.c 与 fw_home_icons.h）。

每个图标是 40x40 的扁平风图标：圆角方块底 + 白色图形。用 4 倍超采样绘制后
用 LANCZOS 缩小，边缘平滑。导出格式为 LVGL 的 LV_COLOR_FORMAT_RGB565A8
（整幅 RGB565 + 整幅 alpha），RGB565 按小端存放（与显示缓冲同字节序，
写屏回调会再交换一次送给 ST7789）。

用法：
    python tools/gen_home_icons.py
"""

import os

from PIL import Image, ImageDraw

SS = 4                      # 超采样倍数
N = 40                      # 图标边长
W = N * SS
WHITE = (255, 255, 255, 255)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_C = os.path.join(ROOT, "main", "framework", "assets", "icons_home.c")
ASSET_H = os.path.join(ROOT, "main", "framework", "include", "fw_home_icons.h")


# ------------------------------- 绘图小工具 -------------------------------

def _box(box):
    return [v * SS for v in box]


def rrect(d, box, radius, **kw):
    d.rounded_rectangle(_box(box), radius=radius * SS, **kw)


def rect(d, box, **kw):
    d.rectangle(_box(box), **kw)


def circle(d, cx, cy, r, **kw):
    d.ellipse(_box([cx - r, cy - r, cx + r, cy + r]), **kw)


def line(d, pts, width, fill=WHITE):
    d.line([(x * SS, y * SS) for x, y in pts], fill=fill,
           width=max(1, int(round(width * SS))), joint="curve")


def poly(d, pts, fill=WHITE):
    d.polygon([(x * SS, y * SS) for x, y in pts], fill=fill)


def arc(d, cx, cy, r, start, end, width, fill=WHITE):
    d.arc(_box([cx - r, cy - r, cx + r, cy + r]), start, end, fill=fill,
          width=max(1, int(round(width * SS))))


def ellipse_box(d, box, **kw):
    d.ellipse(_box(box), **kw)


# --------------------------------- 图形 ---------------------------------
# 坐标都在 1x（40x40）空间里；bg 用于"挖空"（把图形画成底色）

def g_scripts(d, bg):
    line(d, [(16, 13), (10, 20), (16, 27)], 2.6)
    line(d, [(24, 13), (30, 20), (24, 27)], 2.6)
    line(d, [(23.5, 12), (17, 28)], 2.6)


def g_clock(d, bg):
    circle(d, 20, 21, 11, outline=WHITE, width=int(2.4 * SS))
    line(d, [(20, 21), (20, 14.5)], 2.2)
    line(d, [(20, 21), (25.5, 22.5)], 2.2)
    circle(d, 20, 21, 1.7, fill=WHITE)


def g_calendar(d, bg):
    rrect(d, [9, 11, 31, 31], 3, outline=WHITE, width=int(2.3 * SS))
    rect(d, [10.6, 12.6, 29.4, 17.6], fill=WHITE)
    line(d, [(14, 8), (14, 13)], 2.3)
    line(d, [(26, 8), (26, 13)], 2.3)
    circle(d, 14.5, 22, 1.6, fill=WHITE)
    circle(d, 20, 22, 1.6, fill=WHITE)
    circle(d, 25.5, 22, 1.6, fill=WHITE)
    circle(d, 14.5, 26.5, 1.6, fill=WHITE)
    circle(d, 20, 26.5, 1.6, fill=WHITE)


def g_weather(d, bg):
    circle(d, 15.5, 15, 6.2, fill=WHITE)
    for a in range(0, 360, 45):
        line(d, [(15.5 + 8.6 * _cos(a), 15 + 8.6 * _sin(a)),
                 (15.5 + 11.4 * _cos(a), 15 + 11.4 * _sin(a))], 2.0)
    circle(d, 20, 24.5, 6.4, fill=WHITE)
    circle(d, 26.5, 25.5, 5.0, fill=WHITE)
    rrect(d, [13.5, 24, 31.5, 31], 3.4, fill=WHITE)


def g_wifi(d, bg):
    circle(d, 20, 29.5, 2.3, fill=WHITE)
    arc(d, 20, 30.5, 7, 218, 322, 2.5)
    arc(d, 20, 30.5, 12, 218, 322, 2.5)
    arc(d, 20, 30.5, 17, 218, 322, 2.5)


def g_bt(d, bg):
    line(d, [(20, 8.5), (20, 31)], 2.5)
    line(d, [(20, 8.5), (27, 14), (13.5, 24.5)], 2.5)
    line(d, [(20, 31), (27, 25.5), (13.5, 15.5)], 2.5)


def g_display(d, bg):
    rrect(d, [7, 10, 33, 26], 3, outline=WHITE, width=int(2.4 * SS))
    poly(d, [(17.5, 26), (22.5, 26), (24, 30), (16, 30)])
    rrect(d, [12, 30, 28, 32.5], 1.2, fill=WHITE)


def g_sound(d, bg):
    poly(d, [(11, 16.5), (16.5, 16.5), (23, 10.5), (23, 29.5), (16.5, 23.5), (11, 23.5)])
    arc(d, 22, 20, 6.0, -52, 52, 2.3)
    arc(d, 22, 20, 10.0, -52, 52, 2.3)


def g_file(d, bg):
    poly(d, [(8, 11), (16.5, 11), (19, 14.5), (32, 14.5), (32, 30), (8, 30)])
    rect(d, [11, 18, 29, 30], fill=(0, 0, 0, 0))


def g_editor(d, bg):
    rrect(d, [11, 8, 29, 32], 2.5, outline=WHITE, width=int(2.3 * SS))
    line(d, [(15, 15), (25, 15)], 2.0)
    line(d, [(15, 20), (25, 20)], 2.0)
    line(d, [(15, 25), (21, 25)], 2.0)


def g_calc(d, bg):
    rrect(d, [10, 8, 30, 32], 3, outline=WHITE, width=int(2.4 * SS))
    rrect(d, [13.5, 11.5, 26.5, 15.5], 1.4, fill=WHITE)
    for x in (15, 20, 25):
        for y in (20, 25.5):
            circle(d, x, y, 1.7, fill=WHITE)


def g_download(d, bg):
    line(d, [(20, 9), (20, 23)], 2.7)
    poly(d, [(14.5, 20.5), (25.5, 20.5), (20, 27.5)])
    line(d, [(11, 26.5), (11, 31), (29, 31), (29, 26.5)], 2.7)


def g_music(d, bg):
    ellipse_box(d, [11.5, 24.5, 19.5, 31], fill=WHITE)
    line(d, [(18.5, 27.5), (18.5, 10.5)], 2.4)
    poly(d, [(18.5, 9.5), (29.5, 13), (29.5, 18.5), (18.5, 15)])


def g_recorder(d, bg):
    rrect(d, [16, 8, 24, 21], 4, fill=WHITE)
    arc(d, 20, 20, 8.5, 0, 180, 2.4)
    line(d, [(20, 28.5), (20, 32)], 2.4)
    line(d, [(16, 32), (24, 32)], 2.4)


def g_camera(d, bg):
    rrect(d, [7, 12, 33, 30], 3, fill=WHITE)
    rrect(d, [14, 8.5, 22, 12.5], 1.5, fill=WHITE)
    circle(d, 20, 21, 4.8, fill=bg)
    circle(d, 20, 21, 2.0, fill=WHITE)


def g_image(d, bg):
    rrect(d, [8, 10, 32, 30], 2.5, outline=WHITE, width=int(2.4 * SS))
    poly(d, [(12, 26), (18, 18), (22.5, 23), (26, 18.5), (30, 26)])
    circle(d, 15, 15, 2.2, fill=WHITE)


def g_stopwatch(d, bg):
    circle(d, 20, 22, 10.5, outline=WHITE, width=int(2.4 * SS))
    rrect(d, [17.5, 6.5, 22.5, 9.5], 1.2, fill=WHITE)
    line(d, [(26, 11.5), (29, 8.5)], 2.2)
    line(d, [(20, 22), (20, 15.5)], 2.2)
    circle(d, 20, 22, 1.7, fill=WHITE)


def g_timer(d, bg):
    rrect(d, [12, 7.5, 28, 11], 1.6, fill=WHITE)
    rrect(d, [12, 29, 28, 32.5], 1.6, fill=WHITE)
    poly(d, [(14, 12), (26, 12), (20, 20)])
    poly(d, [(14, 28), (26, 28), (20, 20)])


def g_imu(d, bg):
    circle(d, 20, 20.5, 3.0, fill=WHITE)
    line(d, [(20, 20.5), (30, 20.5)], 2.3)
    line(d, [(20, 20.5), (12.5, 12.5)], 2.3)
    line(d, [(20, 20.5), (20, 31)], 2.3)
    poly(d, [(30, 20.5), (25.5, 17.8), (25.5, 23.2)])
    poly(d, [(12.5, 12.5), (14.6, 17.6), (18.4, 14.2)])
    poly(d, [(20, 31), (17.3, 26.6), (22.7, 26.6)])


def g_perf(d, bg):
    arc(d, 20, 23, 11.5, 180, 360, 2.6)
    line(d, [(20, 23), (26.5, 15.5)], 2.3)
    circle(d, 20, 23, 2.0, fill=WHITE)
    line(d, [(11, 23), (11, 26)], 2.0)
    line(d, [(29, 23), (29, 26)], 2.0)


def g_log(d, bg):
    rrect(d, [10, 9, 30, 31], 2.5, outline=WHITE, width=int(2.3 * SS))
    for y in (15, 20, 25):
        circle(d, 14, y, 1.3, fill=WHITE)
        line(d, [(17.5, y), (26, y)], 2.0)


def g_about(d, bg):
    circle(d, 20, 20.5, 11.5, outline=WHITE, width=int(2.4 * SS))
    circle(d, 20, 14, 1.9, fill=WHITE)
    rrect(d, [18.8, 17.5, 21.2, 26.5], 1.2, fill=WHITE)


def _cos(deg):
    import math
    return math.cos(math.radians(deg))


def _sin(deg):
    import math
    return math.sin(math.radians(deg))


# 顺序 = 原始需求「内置应用」列表的顺序
ICONS = [
    ("scripts",   (0x4E, 0x5D, 0x78), (0x39, 0x45, 0x5C), g_scripts),
    ("clock",     (0x4F, 0x9E, 0xFF), (0x2D, 0x6C, 0xD8), g_clock),
    ("calendar",  (0xFF, 0x70, 0x70), (0xE0, 0x3E, 0x3E), g_calendar),
    ("weather",   (0x5B, 0xC8, 0xF5), (0x2D, 0x9C, 0xDB), g_weather),
    ("wifi",      (0x3D, 0xC1, 0x8B), (0x1E, 0x9E, 0x6E), g_wifi),
    ("bt",        (0x6C, 0x74, 0xF0), (0x4A, 0x50, 0xD0), g_bt),
    ("display",   (0xA5, 0x6B, 0xF0), (0x7E, 0x3F, 0xD8), g_display),
    ("sound",     (0xFF, 0x9A, 0x4D), (0xEF, 0x6F, 0x24), g_sound),
    ("file",      (0xFF, 0xC1, 0x4D), (0xF0, 0x9A, 0x18), g_file),
    ("editor",    (0x4D, 0xB0, 0xE0), (0x25, 0x82, 0xB8), g_editor),
    ("calc",      (0x2F, 0xC4, 0xB0), (0x14, 0x9E, 0x8C), g_calc),
    ("download",  (0x59, 0xC4, 0xF5), (0x2A, 0x93, 0xDB), g_download),
    ("music",     (0xEE, 0x6C, 0xA8), (0xD1, 0x3D, 0x84), g_music),
    ("recorder",  (0xF0, 0x5A, 0x54), (0xCC, 0x33, 0x33), g_recorder),
    ("camera",    (0x46, 0x55, 0x69), (0x2B, 0x35, 0x45), g_camera),
    ("image",     (0x8B, 0x74, 0xFF), (0x64, 0x48, 0xE0), g_image),
    ("stopwatch", (0xFF, 0xA7, 0x4D), (0xEE, 0x7E, 0x1E), g_stopwatch),
    ("timer",     (0xC6, 0x8B, 0x5A), (0xA0, 0x67, 0x38), g_timer),
    ("imu",       (0x74, 0x86, 0x9B), (0x4E, 0x5D, 0x70), g_imu),
    ("perf",      (0x3D, 0xC1, 0x8B), (0x1E, 0x9E, 0x6E), g_perf),
    ("log",       (0x8E, 0x9A, 0xA8), (0x66, 0x74, 0x84), g_log),
    ("about",     (0x4F, 0x9E, 0xFF), (0x2D, 0x6C, 0xD8), g_about),
]


def render(bg_top, bg_bottom, glyph):
    """画一个图标，返回 40x40 RGBA。"""
    # 底色：竖向渐变 + 圆角遮罩
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
    img = Image.alpha_composite(img, layer)

    return img.resize((N, N), Image.LANCZOS)


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


def main():
    parts = [
        "/*\n"
        " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
        " *\n"
        " * 桌面 App 彩色图标（自动生成，勿手改）\n"
        " *\n"
        " * 由 tools/gen_home_icons.py 生成：40x40 扁平图标，格式 LV_COLOR_FORMAT_RGB565A8\n"
        " * （整幅 RGB565 + 整幅 alpha）。RGB565 按**小端**存放：LVGL 的图片数据必须与\n"
        " * 显示缓冲同字节序，写屏回调 periph_lcd_flush_cb() 会再交换一次送给 ST7789。\n"
        " * 顺序与原始需求「内置应用」列表一致。\n"
        " */\n\n"
        "#include \"lvgl.h\"\n\n"
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN\n"
        "#define LV_ATTRIBUTE_MEM_ALIGN\n"
        "#endif\n\n",
    ]

    for name, top, bottom, glyph in ICONS:
        img = render(top, bottom, glyph)
        rgb, a8 = encode_a8(img)
        parts.append(c_array("icon_home_%s_map" % name, rgb + a8))
        parts.append("\n")
        parts.append("const lv_image_dsc_t icon_home_%s = {\n" % name)
        parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        parts.append("    .header.cf = LV_COLOR_FORMAT_RGB565A8,\n")
        parts.append("    .header.w = %d,\n" % N)
        parts.append("    .header.h = %d,\n" % N)
        parts.append("    .data_size = %d,\n" % (N * N * 3))
        parts.append("    .data = icon_home_%s_map,\n" % name)
        parts.append("};\n\n")

    with open(ASSET_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))

    with open(ASSET_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/*\n"
            " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
            " *\n"
            " * Framework - Desktop Home Icons\n"
            " *\n"
            " * 桌面 App 彩色图标（自动生成，勿手改），由 tools/gen_home_icons.py 生成。\n"
            " */\n\n"
            "#pragma once\n\n"
            "#include \"lvgl.h\"\n\n"
        )
        for name, _, _, _ in ICONS:
            f.write("LV_IMG_DECLARE(icon_home_%s);\n" % name)

    print("generated: %s" % ASSET_C)
    print("generated: %s" % ASSET_H)
    print("icons: %d" % len(ICONS))

    # 预览图，便于人工确认造型
    sheet = Image.new("RGB", (len(ICONS) * (N + 6) + 6, N + 12), (32, 32, 32))
    for i, (name, top, bottom, glyph) in enumerate(ICONS):
        sheet.paste(render(top, bottom, glyph), (6 + i * (N + 6), 6), render(top, bottom, glyph))
    sheet = sheet.resize((sheet.width * 3, sheet.height * 3), Image.NEAREST)
    preview = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons_home_preview.png")
    sheet.save(preview)
    print("preview: %s" % preview)


if __name__ == "__main__":
    main()
