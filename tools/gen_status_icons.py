"""生成状态栏图标（main/framework/assets/icons_status.c 与 fw_status_icons.h）。

6 个图标：Wi-Fi、蓝牙、声音、录音、摄像头、TF 卡。20x20，形状画成**白色 + alpha**
（RGB565A8），运行时用 LVGL 的 image_recolor 染色：启用时强调色、未启用置灰、
录音中 error 红，深浅主题都只换颜色不换资源。

用 4 倍超采样绘制后 LANCZOS 缩小，边缘平滑。RGB565 按小端存放（与显示缓冲同
字节序，写屏回调会再交换一次送给 ST7789）。

用法：
    python tools/gen_status_icons.py
"""

import os

from PIL import Image, ImageDraw

SS = 4
N = 20
W = N * SS
WHITE = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSET_C = os.path.join(ROOT, "main", "framework", "assets", "icons_status.c")
ASSET_H = os.path.join(ROOT, "main", "framework", "include", "fw_status_icons.h")


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


# --------------------------------- 图形 ---------------------------------
# 坐标在 1x（20x20）空间；c = 图形颜色，bg = 挖空用的"透明"

def g_wifi(d, c, clear):
    circle(d, 10, 15.6, 1.8, c)
    for r, w in ((4.6, 1.5), (8.0, 1.5), (11.4, 1.5)):
        arc(d, 10, 16.4, r, 220, 320, w, c)


def g_bt(d, c, clear):
    line(d, [(10, 3.2), (10, 16.8)], 1.5, c)
    line(d, [(10, 3.2), (14.2, 6.6), (6.4, 12.8)], 1.5, c)
    line(d, [(10, 16.8), (14.2, 13.4), (6.4, 7.2)], 1.5, c)


def g_sound(d, c, clear):
    poly(d, [(4.4, 7.6), (7.6, 7.6), (11.4, 4.2), (11.4, 15.8), (7.6, 12.4), (4.4, 12.4)], c)
    arc(d, 11, 10, 3.2, -50, 50, 1.4, c)
    arc(d, 11, 10, 5.6, -50, 50, 1.4, c)


def g_record(d, c, clear):
    rrect(d, [7.4, 2.4, 12.6, 11.6], 2.6, c)
    arc(d, 10, 10.6, 4.8, 0, 180, 1.5, c)
    line(d, [(10, 15.4), (10, 17.6)], 1.5, c)
    line(d, [(6.8, 17.6), (13.2, 17.6)], 1.5, c)


def g_camera(d, c, clear):
    rrect(d, [1.4, 6.0, 18.6, 16.8], 2.2, c)
    rrect(d, [5.8, 3.4, 11.4, 6.4], 0.9, c)
    circle(d, 10, 11.4, 4.2, clear)        # 镜头外圈挖空
    circle(d, 10, 11.4, 1.7, c)            # 镜心


def g_sd(d, c, clear):
    poly(d, [(4.2, 2.4), (12.6, 2.4), (15.8, 5.6), (15.8, 17.6), (4.2, 17.6)], c)
    rrect(d, [6.6, 4.6, 8.2, 8.0], 0.3, clear)     # 触点槽
    rrect(d, [9.2, 4.6, 10.8, 8.0], 0.3, clear)
    rrect(d, [11.8, 5.6, 13.4, 9.0], 0.3, clear)


ICONS = [
    ("wifi", g_wifi),
    ("bt", g_bt),
    ("sound", g_sound),
    ("record", g_record),
    ("camera", g_camera),
    ("sd", g_sd),
]


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
        " * 状态栏图标（自动生成，勿手改）\n"
        " *\n"
        " * 由 tools/gen_status_icons.py 生成：20x20，形状为白色 + alpha，格式\n"
        " * LV_COLOR_FORMAT_RGB565A8。运行时用 image_recolor 染色（启用强调色 /\n"
        " * 未启用置灰 / 录音中 error 红），所以资源本身不带颜色。\n"
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
        parts.append(c_array("icon_status_%s_map" % name, rgb + a8))
        parts.append("\n")
        parts.append("const lv_image_dsc_t icon_status_%s = {\n" % name)
        parts.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
        parts.append("    .header.cf = LV_COLOR_FORMAT_RGB565A8,\n")
        parts.append("    .header.w = %d,\n" % N)
        parts.append("    .header.h = %d,\n" % N)
        parts.append("    .data_size = %d,\n" % (N * N * 3))
        parts.append("    .data = icon_status_%s_map,\n" % name)
        parts.append("};\n\n")

    with open(ASSET_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(parts))

    with open(ASSET_H, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "/*\n"
            " * SPDX-FileCopyrightText: 2026 SZPI-OS\n"
            " *\n"
            " * Framework - Status Bar Icons\n"
            " *\n"
            " * 状态栏图标（自动生成，勿手改），由 tools/gen_status_icons.py 生成。\n"
            " */\n\n"
            "#pragma once\n\n"
            "#include \"lvgl.h\"\n\n"
        )
        for name, _ in ICONS:
            f.write("LV_IMG_DECLARE(icon_status_%s);\n" % name)

    print("generated: %s" % ASSET_C)
    print("generated: %s" % ASSET_H)

    # 预览：深色主题（强调色 #4F9EFF）与浅色主题（#1D6FD0）各一行
    scale = 8
    pad = 6
    rows = ((0x1E, 0x1E, 0x1E), (0xFF, 0xFF, 0xFF))
    colors = ((0x4F, 0x9E, 0xFF), (0x1D, 0x6F, 0xD0))
    sheet = Image.new("RGB", (pad + len(ICONS) * (N * scale + pad), pad + len(rows) * (N * scale + pad)),
                      (0x88, 0x88, 0x88))
    for ri, (bg, col) in enumerate(zip(rows, colors)):
        band = Image.new("RGB", (len(ICONS) * (N * scale + pad) + pad, N * scale + pad), bg)
        for i, (name, glyph) in enumerate(ICONS):
            im = render(glyph, col + (255,)).resize((N * scale, N * scale), Image.NEAREST)
            band.paste(im, (pad + i * (N * scale + pad), pad // 2), im)
        sheet.paste(band, (0, pad // 2 + ri * (N * scale + pad)))
    preview = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons_status_preview.png")
    sheet.save(preview)
    print("preview: %s" % preview)


if __name__ == "__main__":
    main()
