/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Image（图库）
 *
 * 扫 TF 卡与内置存储里的 PNG / JPEG / GIF / BMP（含 DCIM 等子目录，最多 IMG_MAX 张），
 * 网格点选后全屏查看：图片铺满整屏，文件名与序号悬浮左上，底部悬浮
 * 「上一张 / 缩小 / 倍数 / 放大 / 下一张」，最多放大到 6 倍看细节（放大后拖动看不同
 * 位置），点倍数回到"适应屏幕"。JPEG 由 LVGL 的 TJPGD 解码器支持（已开 LV_USE_TJPGD）。
 *
 * 缩放有个前提：LVGL 只对"整幅已解码"的图片做缩放绘制，逐行解码的图片会被直接拒绝
 * （lv_draw_image.c 的 "Partially decoded images cannot be transformed"）。相机拍的是
 * BMP，而 LVGL 的 BMP 解码器正是逐行解码的，所以这里把 BMP 自己解成一张 RAM 里的
 * RGB565 图（LVGL 的"变量图片"），LVGL 就当作整幅已解码，缩放与拖动都能用。JPEG 同样是
 * 逐行解码、又没法自己解，只能按原始大小显示并拖动；PNG 的解码器整幅解完，可以缩放。
 *
 * 触摸是单点、界面不使用滑动手势，切换图片用按钮而不是左右滑；放大后拖动查看是滚动。
 * 文件路径放堆上（IMG_MAX × IMG_PATH_MAX = 4.8 KB），不在 App 里放大静态数组。
 */

#include "app_image.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.image";

#define IMG_INIT       32       /* 图片列表起始容量（按需成倍扩） */
#define IMG_MAX        256      /* 上限：再多网格也建不动，防御性拦截 */
#define IMG_BUILD_CHUNK 6       /* 分段构建：每批建几个格子（一个格子三个控件） */
#define IMG_PATH_MAX   200      /* 路径缓冲，与文件管理的 PATH_MAX_LEN 一致 */
#define IMG_SCAN_DEPTH 3        /* 目录递归层数（/sdcard/DCIM/100MEDIA 这类要够） */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_hint = NULL;         /* 头部：图片数量 / 空目录提示 */
static lv_obj_t *s_grid = NULL;

static lv_obj_t *s_viewer = NULL;       /* 全屏查看层（盖住内容区） */
static lv_obj_t *s_area = NULL;         /* 图片容器：原图模式下拖动它看超出部分 */
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_view_name = NULL;    /* 查看层顶部文件名 */
static lv_obj_t *s_view_pos = NULL;     /* 查看层顶部序号 */
static lv_obj_t *s_view_hint = NULL;    /* 解码不了时的提示（渐进式 JPEG 之类） */
static lv_obj_t *s_zoom_label = NULL;   /* 底部"倍数"按钮里的标签（点一下复位） */
static lv_obj_t *s_btn_zoom_out = NULL; /* 底部三个缩放控件：不能缩放的格式藏起来 */
static lv_obj_t *s_btn_zoom_in = NULL;
static lv_obj_t *s_btn_zoom_reset = NULL;
static int s_zoom = 100;                /* 当前档位，100 = 适应屏幕 */

/* BMP 自己解码后的"变量图片"：像素在 PSRAM，整幅交给 LVGL，缩放绘制才有效 */
static lv_image_dsc_t s_dsc;
static uint8_t *s_ram = NULL;
static bool s_zoomable = false;

/* 图片完整路径放堆上（按目录内容成倍扩），不在 App 里放大静态数组 */
static char (*s_paths)[IMG_PATH_MAX] = NULL;
static size_t s_paths_cap = 0;
static size_t s_count = 0;
static int s_index = -1;
static fw_ui_stage_t *s_stage = NULL;       /* 网格分段构建 */

/* 前置声明 */
static void scan(void);
static void open_index(int idx);
static void tile_cb(lv_event_t *e);
static void refresh_cb(lv_event_t *e);

/* ------------------------------- 小工具 ------------------------------- */

/* 保证图片列表至少能放 need 项；不够就成倍扩（上限 IMG_MAX） */
static bool paths_reserve(size_t need)
{
    if (need <= s_paths_cap) return true;
    if (need > IMG_MAX) return false;

    size_t cap = (s_paths_cap > 0) ? s_paths_cap : IMG_INIT;
    while (cap < need) cap *= 2;
    if (cap > IMG_MAX) cap = IMG_MAX;

    void *p = realloc(s_paths, cap * IMG_PATH_MAX);
    if (p == NULL) return false;

    s_paths = p;
    s_paths_cap = cap;
    return true;
}

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;

    for (size_t i = 0; ext[i] != '\0'; i++) {
        char a = dot[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[i]) return false;
    }
    return true;
}

static bool is_image(const char *name)
{
    return has_ext(name, ".png") || has_ext(name, ".jpg") || has_ext(name, ".jpeg") ||
           has_ext(name, ".gif") || has_ext(name, ".bmp");
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/* 所在位置（网格第二行）：一级子目录名，根目录直接标存储 */
static const char *path_where(const char *path)
{
    static char buf[32];

    if (strncmp(path, "/sdcard", 7) != 0) return "内置存储";

    const char *rest = path + 7;
    if (rest[0] != '/') return "TF 卡";
    rest++;

    const char *slash = strchr(rest, '/');
    if (slash == NULL) return "TF 卡";

    size_t n = (size_t)(slash - rest);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, rest, n);
    buf[n] = '\0';
    return buf;
}

/* ------------------------------- 扫描 ------------------------------- */

/* 递归扫一个目录（层数与张数都有上限，名单放堆上） */
static void scan_dir(const char *dir, int depth)
{
    if (depth > IMG_SCAN_DEPTH || s_count >= IMG_MAX) return;

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) return;

    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL) {
        if (s_count >= IMG_MAX) break;

        if (entry->is_dir) {
            if (entry->name[0] == '.') continue;    /* 隐藏目录 / 系统目录 */

            char sub[IMG_PATH_MAX];
            if (snprintf(sub, sizeof(sub), "%s/%s", dir, entry->name) >= (int)sizeof(sub)) continue;
            scan_dir(sub, depth + 1);
            continue;
        }

        if (!is_image(entry->name)) continue;
        if (!paths_reserve(s_count + 1)) break;     /* 到上限或内存不足：停在这里 */

        char full[IMG_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, entry->name) >= (int)sizeof(full)) continue;
        strlcpy(s_paths[s_count], full, IMG_PATH_MAX);
        s_count++;
    }

    svc_storage_iter_end(it);
}

static void update_hint(void)
{
    if (s_hint == NULL) return;

    char buf[48];
    if (s_count == 0) {
        strlcpy(buf, "没有找到图片（TF 卡 / 内置存储）", sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "共 %u 张图片", (unsigned)s_count);
    }

    lvgl_port_lock(0);
    lv_label_set_text(s_hint, buf);
    lvgl_port_unlock();
}

static int32_t s_keep_y = 0;             /* 重建网格时要保持的滚动位置 */

/* 长按删除：先把目标拷出来（网格重建后会失效），确认后删掉并重扫 */
static char s_del_path[IMG_PATH_MAX] = "";
static char s_del_name[IMG_PATH_MAX] = "";

static void del_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    if (svc_storage_remove(s_del_path) != ESP_OK) {
        fw_ui_toast("删除失败", 2000);
        return;
    }

    fw_ui_toast("已删除", 1500);
    ESP_LOGI(TAG, "removed %s", s_del_path);

    scan();                                 /* 网格去掉这一张 */
}

static void tile_long_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_count) return;

    strlcpy(s_del_path, s_paths[idx], sizeof(s_del_path));
    strlcpy(s_del_name, path_basename(s_paths[idx]), sizeof(s_del_name));

    char msg[IMG_PATH_MAX + 64];
    snprintf(msg, sizeof(msg), "删除「%s」？删除后无法恢复。", s_del_name);
    fw_ui_dialog(NULL, "删除图片", msg, FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 del_confirm_cb, NULL);
}

/* 分段建第 idx 个格子（由 fw_ui_stage_start 调，已在 LVGL 锁内） */
static void build_tile(size_t idx, void *user)
{
    (void)user;
    if (s_grid == NULL || idx >= s_count) return;

    lv_obj_t *tile = lv_button_create(s_grid);
    lv_obj_set_size(tile, 135, 96);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_style_bg_color(tile, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(tile, 10, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(tile, 6, 0);
    lv_obj_set_style_pad_row(tile, 2, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_add_event_cb(tile, tile_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)idx);

    fw_ui_icon(tile, &icon_ui_file_image, fw_theme_color_accent());

    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, path_basename(s_paths[idx]));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_font(name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(name, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *where = lv_label_create(tile);
    lv_label_set_text(where, path_where(s_paths[idx]));
    lv_label_set_long_mode(where, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(where, lv_pct(100));
    lv_obj_set_style_text_font(where, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(where, fw_theme_color_text_secondary(), 0);
    lv_obj_set_style_text_align(where, LV_TEXT_ALIGN_CENTER, 0);
}

/* 全部建完：把滚动位置放回去 */
static void build_done(void *user)
{
    (void)user;
    if (s_grid == NULL || s_keep_y <= 0) return;

    lvgl_port_lock(0);
    lv_obj_update_layout(s_grid);
    lv_obj_scroll_to_y(s_grid, s_keep_y, LV_ANIM_OFF);
    lvgl_port_unlock();
}

/* 按 s_paths / s_count 重建网格（扫描和"从文件管理直接打开单张"都用它）。
 * 格子多，分段建；重建前记住滚动位置，重新进前台不会跳回顶部 */
static void fill_grid(void)
{
    if (s_grid == NULL) return;

    lvgl_port_lock(0);

    s_keep_y = lv_obj_get_scroll_y(s_grid);
    lv_obj_clean(s_grid);
    fw_ui_stage_start(s_stage, s_count, IMG_BUILD_CHUNK, build_tile, build_done, NULL);

    lvgl_port_unlock();
}

static void scan(void)
{
    s_count = 0;
    s_index = -1;

    if (s_paths != NULL) {
        scan_dir("/sdcard", 1);
        scan_dir("/internal", 1);
    }

    fill_grid();
    update_hint();

    ESP_LOGI(TAG, "scan: %u image(s)", (unsigned)s_count);
}

/* ------------------------------ BMP 解码 ------------------------------ */

#define BMP_MAX_W      4096                     /* 宽高与总像素上限：再大就不自己解了 */
#define BMP_MAX_H      4096
#define BMP_MAX_PIXELS (1280 * 960)

static uint32_t bmp_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t bmp_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 把无压缩 BMP 解成 LVGL 的 RGB565（小端）像素缓冲，成功返回 malloc 出来的缓冲
 * （用完要 free）并给出宽高，格式不认识返回 NULL。
 *
 * 只认 24 / 32 位无压缩（相机写出来的是 24 位、自下而上）；行按 4 字节对齐，高度为负
 * 表示自上而下。BMP 里像素的字节序是 B、G、R，正好是 LVGL RGB888 的顺序。
 * 一次只读一行，行缓冲很小，整块内存只花在输出的像素上。 */
static uint8_t *load_bmp(const char *path, int32_t *out_w, int32_t *out_h)
{
    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK) return NULL;

    uint8_t hdr[54];
    uint32_t rn = 0;
    if (lv_fs_read(&f, hdr, sizeof(hdr), &rn) != LV_FS_RES_OK || rn != sizeof(hdr) ||
        hdr[0] != 'B' || hdr[1] != 'M') {
        lv_fs_close(&f);
        return NULL;
    }

    const uint32_t data_off = bmp_rd32(hdr + 10);
    const uint32_t dib      = bmp_rd32(hdr + 14);
    const int32_t  w        = (int32_t)bmp_rd32(hdr + 18);
    const int32_t  raw_h    = (int32_t)bmp_rd32(hdr + 22);
    const uint16_t bpp      = bmp_rd16(hdr + 28);
    const uint32_t comp     = bmp_rd32(hdr + 30);
    const int32_t  h        = (raw_h < 0) ? -raw_h : raw_h;

    if (dib < 40 || comp != 0 || (bpp != 24 && bpp != 32) ||
        w <= 0 || h <= 0 || w > BMP_MAX_W || h > BMP_MAX_H ||
        (int64_t)w * h > BMP_MAX_PIXELS) {
        lv_fs_close(&f);
        return NULL;
    }

    const uint32_t step     = (uint32_t)(bpp / 8);
    const uint32_t row_size = (((uint32_t)w * step) + 3u) & ~3u;

    uint8_t *pixels = malloc((size_t)w * 2u * (size_t)h);
    uint8_t *row    = malloc(row_size);
    if (pixels == NULL || row == NULL) {
        free(pixels);
        free(row);
        lv_fs_close(&f);
        return NULL;
    }

    bool ok = true;
    for (int32_t y = 0; y < h; y++) {
        const uint32_t src_y = (raw_h < 0) ? (uint32_t)y : (uint32_t)(h - 1 - y);
        if (lv_fs_seek(&f, data_off + row_size * src_y, LV_FS_SEEK_SET) != LV_FS_RES_OK ||
            lv_fs_read(&f, row, row_size, &rn) != LV_FS_RES_OK || rn != row_size) {
            ok = false;
            break;
        }

        uint16_t *dst = (uint16_t *)pixels + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * step;
            dst[x] = (uint16_t)(((uint16_t)(p[2] & 0xF8) << 8) |
                                ((uint16_t)(p[1] & 0xFC) << 3) |
                                ((uint16_t)(p[0] & 0xF8) >> 3));
        }
    }

    free(row);
    lv_fs_close(&f);

    if (!ok) {
        free(pixels);
        return NULL;
    }

    *out_w = w;
    *out_h = h;
    return pixels;
}

/* ------------------------------ 整幅解码 ------------------------------ */

/* 全尺寸解码的上限（像素）：整幅解码结果是 ARGB8888（4 字节/像素），
 * 超过这个量级就会超出图片缓存、每帧都要重解，不如不开 */
#define IMG_MAX_DECODE_PX   (1024 * 768)

/* 把一张 RAM 图（已解成 RGB565）交给图片对象，句柄留给 close_viewer 释放 */
static void set_ram_image(uint8_t *pixels, int32_t w, int32_t h)
{
    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w      = (uint32_t)w;
    s_dsc.header.h      = (uint32_t)h;
    s_dsc.header.stride = (uint32_t)w * 2u;
    s_dsc.data_size     = (uint32_t)w * 2u * (uint32_t)h;
    s_dsc.data          = pixels;
    s_ram = pixels;

    lv_image_set_src(s_img, &s_dsc);
}

/* ------------------------------ 查看层 ------------------------------ */

/* 顶部：文件名 + 序号；顺带把这张图设给查看器 */
static void show_current(void)
{
    if (s_view_name == NULL || s_view_pos == NULL) return;
    if (s_index < 0 || (size_t)s_index >= s_count) return;

    uint8_t *old = s_ram;                   /* 上一张的解码缓冲：设好新图之后再放 */
    s_ram = NULL;
    s_zoomable = false;
    memset(&s_dsc, 0, sizeof(s_dsc));

    lvgl_port_lock(0);

    char path[IMG_PATH_MAX + 4];
    bool ok = false;
    const char *hint = "无法显示这张图片";

    if (s_img != NULL && fw_asset_fs_path(s_paths[s_index], path, sizeof(path)) == ESP_OK) {
        const char *name = s_paths[s_index];

        /* BMP 自己解成 RAM 图：LVGL 的 BMP 解码器逐行解码，交给它就没法缩放 */
        if (has_ext(name, ".bmp")) {
            int32_t w = 0, h = 0;
            uint8_t *px = load_bmp(path, &w, &h);
            if (px != NULL) {
                set_ram_image(px, w, h);
                ok = true;
                s_zoomable = true;
                ESP_LOGI(TAG, "show %s: bmp %dx%d -> rgb565", path, (int)w, (int)h);
            } else {
                ESP_LOGW(TAG, "show %s: bmp decode failed, fall back", path);
            }
        }

        if (!ok) {
            /* 先拿尺寸：整幅解码的图（PNG）超过上限就直接不开 —— 全尺寸解码动辄几 MB，
             * 图片缓存放不下，之后每帧都要重解，界面会卡成幻灯片 */
            lv_image_header_t hdr;
            const lv_result_t info = lv_image_decoder_get_info(path, &hdr);
            /* 只有"整幅解码"的格式（PNG）才受尺寸上限约束：JPEG / BMP 是逐行解码，
             * 大图也只是按 1:1 显示、只解可见那几行，不该拦 */
            const bool full_decode = has_ext(name, ".png");

            if (info != LV_RESULT_OK) {
                /* 解不了的格式：渐进式 JPEG 这类 TJPGD 不支持，别给一片空白 */
                hint = (has_ext(name, ".jpg") || has_ext(name, ".jpeg"))
                           ? "这张 JPEG 解不开（只支持基线 JPEG）"
                           : "无法显示这张图片";
                ESP_LOGW(TAG, "show %s: no decoder", path);
            } else if (full_decode && (uint64_t)hdr.w * hdr.h > IMG_MAX_DECODE_PX) {
                hint = "图片太大，打不开";
                ESP_LOGW(TAG, "show %s: %ux%u exceeds the limit", path,
                         (unsigned)hdr.w, (unsigned)hdr.h);
            } else {
                /* 交给 LVGL 按路径解码：整幅解码的（PNG）只解一次就被图片缓存住，
                 * 之后每帧命中缓存，不会每帧重解（缓存大小见 CONFIG_LV_CACHE_DEF_SIZE）；
                 * 逐行解码的（JPEG）只能按原始大小 1:1 显示 */
                lv_image_set_src(s_img, path);
                ok = true;
                s_zoomable = full_decode;       /* 只有整幅解码的格式能缩放 */
                ESP_LOGI(TAG, "show %s: %ux%u %s", path, (unsigned)hdr.w, (unsigned)hdr.h,
                         full_decode ? "scalable" : "1:1");
            }
        }
    }

    if (s_img != NULL) {
        if (ok) {
            lv_obj_set_hidden(s_img, false);
            if (s_view_hint != NULL) lv_obj_set_hidden(s_view_hint, true);
        } else {
            lv_image_set_src(s_img, NULL);
            lv_obj_set_hidden(s_img, true);
            if (s_view_hint != NULL) {
                lv_label_set_text(s_view_hint, hint);
                lv_obj_set_hidden(s_view_hint, false);
            }
        }
    }

    lv_label_set_text(s_view_name, path_basename(s_paths[s_index]));

    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%u", s_index + 1, (unsigned)s_count);
    lv_label_set_text(s_view_pos, pos);

    lvgl_port_unlock();

    free(old);                              /* 图片对象已指向新图，旧缓冲不会再被读到 */
}

/* ------------------------------ 缩放 ------------------------------ */

/* 缩放档位，相对"适应屏幕"（100 = 适应）。放大后图片装不下时，可在图片区拖动查看 */
static const int ZOOM_STEPS[] = { 100, 150, 200, 300, 400, 600 };

/* 按当前档位重排图片。
 *
 * 能缩放的图（自己解出来的 BMP、整幅解码的 PNG）用"对象尺寸 + STRETCH"：STRETCH 会按
 * 对象尺寸反推 scale，所以"缩放"就是重新设对象尺寸（对齐是 STRETCH 时 LVGL 会忽略
 * lv_image_set_scale）。放大到装不下就把图片区变成可滚动，手指拖动看不同位置。
 * 不能缩放的图（JPEG 等逐行解码的）按原始大小显示，同样拖动看超出部分，缩放按钮藏起来。 */
static void apply_zoom(void)
{
    if (s_img == NULL || s_area == NULL) return;

    lvgl_port_lock(0);
    lv_obj_update_layout(s_area);

    /* 每次重排都先把滚动位置归零：摆放位置是按"当前滚动量"算的（lv_obj_move_to 会减掉
     * 它），带着上次拖出来的偏移重新摆放，图会偏到屏幕外去 */
    lv_obj_scroll_to(s_area, 0, 0, LV_ANIM_OFF);

    const int32_t aw = lv_obj_get_width(s_area);
    const int32_t ah = lv_obj_get_height(s_area);
    const int32_t iw = lv_image_get_src_width(s_img);
    const int32_t ih = lv_image_get_src_height(s_img);

    if (!s_zoomable || iw <= 0 || ih <= 0) {
        lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_size(s_img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(s_img, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_scrollable(s_area, true);

        if (s_btn_zoom_out != NULL) lv_obj_set_hidden(s_btn_zoom_out, true);
        if (s_btn_zoom_in != NULL) lv_obj_set_hidden(s_btn_zoom_in, true);
        if (s_btn_zoom_reset != NULL) lv_obj_set_hidden(s_btn_zoom_reset, true);

        lvgl_port_unlock();
        return;
    }

    /* 适应屏幕：能完整放下，且不放大（256 = 1:1） */
    int32_t fit = 256;
    if (aw > 0 && ah > 0) {
        const int32_t sx = aw * 256 / iw;
        const int32_t sy = ah * 256 / ih;
        fit = (sx < sy) ? sx : sy;
        if (fit > 256) fit = 256;
    }

    const int32_t scale = fit * s_zoom / 100;
    const int32_t sw = (iw * scale) >> 8;
    const int32_t sh = (ih * scale) >> 8;

    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(s_img, sw, sh);

    if (sw <= aw && sh <= ah) {
        lv_obj_set_scrollable(s_area, false);
        lv_obj_center(s_img);
    } else {
        /* 装不下：拖动查看。这里必须用 TOP_LEFT 明确改掉对齐方式 —— 上一张如果是
         * lv_obj_center 摆的，对齐还留着 CENTER，而 lv_obj_set_pos 只改 x/y 不改对齐，
         * 图会被摆在中间、滚动量又停在 0，左边那截就再也拖不出来了 */
        lv_obj_set_scrollable(s_area, true);
        lv_obj_align(s_img, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    if (s_btn_zoom_out != NULL) lv_obj_set_hidden(s_btn_zoom_out, false);
    if (s_btn_zoom_in != NULL) lv_obj_set_hidden(s_btn_zoom_in, false);
    if (s_btn_zoom_reset != NULL) lv_obj_set_hidden(s_btn_zoom_reset, false);

    if (s_zoom_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%dx", s_zoom / 100, (s_zoom % 100) / 10);
        lv_label_set_text(s_zoom_label, buf);
    }

    lvgl_port_unlock();
}

static void zoom_step(int dir)
{
    if (!s_zoomable) return;

    const size_t n = sizeof(ZOOM_STEPS) / sizeof(ZOOM_STEPS[0]);
    size_t i = 0;
    while (i < n && ZOOM_STEPS[i] != s_zoom) i++;

    if (i >= n) {
        s_zoom = 100;
    } else if (dir > 0 && i + 1 < n) {
        s_zoom = ZOOM_STEPS[i + 1];
    } else if (dir < 0 && i > 0) {
        s_zoom = ZOOM_STEPS[i - 1];
    }

    apply_zoom();
}

static void zoom_in_cb(lv_event_t *e)
{
    (void)e;
    zoom_step(1);
}

static void zoom_out_cb(lv_event_t *e)
{
    (void)e;
    zoom_step(-1);
}

/* 倍数按钮：点一下回到"适应屏幕" */
static void zoom_reset_cb(lv_event_t *e)
{
    (void)e;
    if (!s_zoomable) return;

    s_zoom = 100;
    apply_zoom();
}

static void close_viewer(void)
{
    if (s_viewer == NULL) return;

    lvgl_port_lock(0);
    lv_obj_delete(s_viewer);
    s_viewer = NULL;
    s_area = NULL;
    s_img = NULL;
    s_view_name = NULL;
    s_view_pos = NULL;
    s_view_hint = NULL;
    s_zoom_label = NULL;
    s_btn_zoom_out = NULL;
    s_btn_zoom_in = NULL;
    s_btn_zoom_reset = NULL;
    lvgl_port_unlock();

    free(s_ram);                            /* 图片对象已经删掉，解码缓冲不会再被引用 */
    s_ram = NULL;
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;

    s_index = (s_index <= 0) ? (int)s_count - 1 : s_index - 1;
    s_zoom = 100;                           /* 换图回到"适应屏幕" */
    show_current();
    apply_zoom();
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;

    s_index = ((size_t)(s_index + 1) >= s_count) ? 0 : s_index + 1;
    s_zoom = 100;
    show_current();
    apply_zoom();
}

/* 查看层的公共行容器：透明底、不滚动、横向排 */
static lv_obj_t *view_row(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void build_viewer(void)
{
    /* 整屏都是图片：查看层不留边距，控件都悬浮在图片上方 */
    s_viewer = lv_obj_create(s_root);
    lv_obj_set_size(s_viewer, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H);
    lv_obj_set_pos(s_viewer, 0, FW_STATUSBAR_H);
    lv_obj_set_scrollable(s_viewer, false);
    lv_obj_set_style_bg_color(s_viewer, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(s_viewer, 0, 0);
    lv_obj_set_style_border_width(s_viewer, 0, 0);

    /* 图片区：铺满整屏；放大后图片比它大，就能拖动 */
    s_area = lv_obj_create(s_viewer);
    lv_obj_set_size(s_area, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_area, 0, 0);
    lv_obj_set_style_bg_opa(s_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_area, 0, 0);
    lv_obj_set_style_pad_all(s_area, 0, 0);
    lv_obj_set_scroll_dir(s_area, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(s_area, LV_SCROLLBAR_MODE_OFF);

    s_img = lv_image_create(s_area);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);   /* 摆放方式由 apply_zoom 按图改 */
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 0);

    s_view_hint = lv_label_create(s_area);
    lv_label_set_text(s_view_hint, "无法显示这张图片");
    lv_obj_set_style_text_font(s_view_hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_view_hint, fw_theme_color_text_secondary(), 0);
    lv_obj_center(s_view_hint);
    lv_obj_set_clickable(s_view_hint, false);
    lv_obj_set_hidden(s_view_hint, true);

    /* 悬浮左上：文件名 + 序号（半透明卡片底，压在图片上也看得清）。
     * 悬浮层一律不吃触摸，否则压在图上就挡住了拖动 —— 只有里面的按钮是可点的 */
    lv_obj_t *pill = lv_obj_create(s_viewer);
    lv_obj_set_size(pill, 240, 26);
    lv_obj_align(pill, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_scrollable(pill, false);
    lv_obj_set_clickable(pill, false);
    lv_obj_set_style_bg_color(pill, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_90, 0);
    lv_obj_set_style_radius(pill, 8, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_hor(pill, 6, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_view_name = lv_label_create(pill);
    lv_obj_set_flex_grow(s_view_name, 1);
    lv_label_set_long_mode(s_view_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_view_name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_view_name, fw_theme_color_text_primary(), 0);
    lv_obj_set_clickable(s_view_name, false);

    s_view_pos = lv_label_create(pill);
    lv_obj_set_style_text_font(s_view_pos, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_view_pos, fw_theme_color_text_secondary(), 0);
    lv_obj_set_clickable(s_view_pos, false);

    /* 悬浮底部：上一张 / 缩小 / 倍数(点一下复位) / 放大 / 下一张 */
    lv_obj_t *bar = lv_obj_create(s_viewer);
    lv_obj_set_size(bar, lv_pct(100), 32);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    fw_ui_icon_btn(bar, &icon_ui_left2, NULL, 46, prev_cb, NULL);

    s_btn_zoom_out = fw_ui_icon_btn(bar, &icon_ui_zoom_out, NULL, 46, zoom_out_cb, NULL);
    s_btn_zoom_reset = fw_ui_btn(bar, "1.0x", 58, false, zoom_reset_cb, NULL);
    s_zoom_label = lv_obj_get_child(s_btn_zoom_reset, 0);
    s_btn_zoom_in = fw_ui_icon_btn(bar, &icon_ui_zoom_in, NULL, 46, zoom_in_cb, NULL);

    fw_ui_icon_btn(bar, &icon_ui_right2, NULL, 46, next_cb, NULL);
}

static void open_index(int idx)
{
    if (idx < 0 || (size_t)idx >= s_count) return;

    /* 已经开着查看层（例如从文件管理又发来一张）：先关掉旧的，别叠起来 */
    close_viewer();
    s_index = idx;
    s_zoom = 100;                           /* 每次打开都从"适应屏幕"开始 */

    lvgl_port_lock(0);
    build_viewer();
    show_current();                         /* 设好图片并算出"适应"系数 */
    apply_zoom();
    lvgl_port_unlock();
}

static void tile_cb(lv_event_t *e)
{
    open_index((int)(intptr_t)lv_event_get_user_data(e));
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    scan();
}

/* 来自文件管理（"Image?path=/xxx.png"）：直接打开这张图。在扫描结果里就跳到它
 * （上一张 / 下一张能接着翻），不在就加进列表末尾（列表满了才缩成这一张）。 */
static bool open_external(void)
{
    const char *args = fw_app_mgr_get_args();
    const char *p = (args != NULL) ? strstr(args, "path=") : NULL;

    if (p == NULL) return false;

    p += 5;
    if (p[0] != '/' || !is_image(p)) return false;

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_paths[i], p) == 0) {
            open_index((int)i);
            return true;
        }
    }

    if (s_paths == NULL || strlen(p) >= IMG_PATH_MAX) return false;

    if (paths_reserve(s_count + 1)) {
        strlcpy(s_paths[s_count], p, IMG_PATH_MAX);
        s_count++;
        fill_grid();
        update_hint();
        open_index((int)s_count - 1);
        return true;
    }

    /* 列表满了（或内存不够）：临时只放这一张 */
    s_count = 1;
    s_index = -1;
    strlcpy(s_paths[0], p, IMG_PATH_MAX);
    fill_grid();
    update_hint();
    open_index(0);
    return true;
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *image_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_paths == NULL) {
        paths_reserve(IMG_INIT);
    }
    if (s_paths == NULL) {
        fw_ui_toast("内存不足", 2000);
    }

    if (s_stage == NULL) {
        s_stage = fw_ui_stage_create();
    }

    lv_obj_t *head = view_row(body, 28);
    s_hint = lv_label_create(head);
    lv_obj_set_flex_grow(s_hint, 1);
    lv_obj_set_style_text_font(s_hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_hint, fw_theme_color_text_secondary(), 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_MODE_DOTS);
    fw_ui_icon_btn(head, &icon_ui_refresh, NULL, 40, refresh_cb, NULL);

    s_grid = fw_ui_grid(body, 2, 135, 96);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_scrollable(s_grid, true);        /* 图多时网格自己滚动 */

    lvgl_port_unlock();

    /* 界面建好后填内容：扫图片，再处理文件管理传来的 path= */
    scan();
    open_external();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* 重新进前台：重扫一遍（别的 App 可能刚存了图片），并打开文件管理刚点的那张。
 * 参数只在这里处理（on_create 管首次创建）—— 这样反复点同一张每次都会打开。
 * 查看层开着时不重扫：列表一换，序号与前后翻页就对不上了 */
static void image_on_resume(void *ctx)
{
    (void)ctx;

    if (s_viewer == NULL) scan();
    open_external();
}

static void image_on_destroy(void *ctx)
{
    (void)ctx;

    /* 先停分段构建：回调会用到下面要删的网格 */
    fw_ui_stage_stop(s_stage);

    close_viewer();

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_hint = NULL;
    s_grid = NULL;
    lvgl_port_unlock();

    free(s_paths);
    s_paths = NULL;
    s_paths_cap = 0;
    s_count = 0;
    s_index = -1;
    fw_ui_stage_destroy(s_stage);
    s_stage = NULL;
}

/* 查看层打开时，返回键先关它（返回 true 表示 App 内已处理） */
static bool image_on_back(void *ctx)
{
    (void)ctx;

    if (s_viewer != NULL) {
        close_viewer();
        return true;
    }
    return false;
}

const fw_app_desc_t app_image_desc = {
    .name = "Image",
    .title = "图库",
    .icon = &icon_home_image,
    .on_create = image_on_create,
    .on_resume = image_on_resume,
    .on_destroy = image_on_destroy,
    .on_back = image_on_back,
};
