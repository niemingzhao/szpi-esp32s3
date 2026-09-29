/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Video（APP-VIDEO 视频播放器）
 *
 * PRD 要的是 TF 卡 MJPEG（MM-009）。LVGL 8.3 只带 PNG / GIF / BMP 解码器，没有 JPEG
 * 解码，所以第一版做成**帧序列播放**：目录里放 frame_0001.bmp / frame_0001.png … 就能
 * 当视频播（自动播放、暂停、逐帧、进度）。列表里的 .mjpeg / .avi 只登记，点开会提示
 * 需要 MM-009 解码器。
 *
 * 帧路径不缓存：只记首帧名（前缀 + 起始序号 + 位宽 + 后缀），每帧按序号拼文件名，
 * 存在性交给 LVGL 打开时判断，这样 App 里的静态内存只有几百字节。
 */

#include "app_video.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "app.video";

#define VID_FPS_DEFAULT   5
#define VID_ROW_MAX       12
#define VID_ROW_LEN       96
#define VID_FRAME_MAX     600
#define VID_PATH_MAX      300

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_player = NULL;
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_frame_label = NULL;
static lv_obj_t *s_play_label = NULL;
static lv_timer_t *s_timer = NULL;

static char s_rows[VID_ROW_MAX][VID_ROW_LEN];
static size_t s_row_count = 0;

/* 当前帧序列描述 */
static char s_seq_dir[VID_ROW_LEN];
static char s_seq_prefix[32];
static char s_seq_ext[8];
static int s_seq_first = 1;
static int s_seq_width = 4;
static int s_seq_count = 0;
static int s_seq_index = 0;
static bool s_playing = false;

/* ------------------------------- 帧序列 ------------------------------- */

static void frame_path_of(int idx, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s%0*d%s", s_seq_dir, s_seq_prefix,
             s_seq_width, s_seq_first + idx, s_seq_ext);
}

/* 从 "frame_0001.bmp" 里拆出前缀 / 序号 / 位宽 / 后缀 */
static bool parse_frame_name(const char *name, char *prefix, size_t plen,
                             int *num, int *width, char *ext, size_t elen)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;

    if (strcasecmp(dot, ".bmp") != 0 && strcasecmp(dot, ".png") != 0) return false;

    size_t ext_len = strlen(dot);
    if (ext_len >= elen) return false;
    strlcpy(ext, dot, elen);

    int end = (int)(dot - name);
    int start = end;
    while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9') start--;
    if (start == end) return false;

    size_t pre = (size_t)start;
    if (pre >= plen) pre = plen - 1;
    memcpy(prefix, name, pre);
    prefix[pre] = '\0';

    *width = end - start;
    *num = atoi(name + start);
    return true;
}

/* 打开目录作为帧序列，返回帧数（0 = 不是帧序列） */
static int open_sequence(const char *dir)
{
    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) return 0;

    int count = 0;
    bool found = false;
    char prefix[32] = {0};
    char ext[8] = {0};
    int num = 0, width = 0;

    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL) {
        if (entry->is_dir) continue;

        char p[32], e[8];
        int n = 0, w = 0;
        if (!parse_frame_name(entry->name, p, sizeof(p), &n, &w, e, sizeof(e))) continue;

        if (!found) {
            strlcpy(prefix, p, sizeof(prefix));
            strlcpy(ext, e, sizeof(ext));
            num = n;
            width = w;
            found = true;
        }
        count++;
        if (count >= VID_FRAME_MAX) break;
    }
    svc_storage_iter_end(it);

    if (!found || count < 2) return 0;

    strlcpy(s_seq_dir, dir, sizeof(s_seq_dir));
    strlcpy(s_seq_prefix, prefix, sizeof(s_seq_prefix));
    strlcpy(s_seq_ext, ext, sizeof(s_seq_ext));
    s_seq_first = num;
    s_seq_width = width;
    s_seq_count = count;
    s_seq_index = 0;
    return count;
}

static void show_frame(void)
{
    if (s_img == NULL || s_frame_label == NULL || s_seq_count == 0) return;

    char path[VID_PATH_MAX];
    char fs_path[VID_PATH_MAX + 4];
    frame_path_of(s_seq_index, path, sizeof(path));
    if (fw_asset_fs_path(path, fs_path, sizeof(fs_path)) == ESP_OK) {
        lv_img_set_src(s_img, fs_path);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", s_seq_index + 1, s_seq_count);
    lv_label_set_text(s_frame_label, buf);
}

static void step(int delta)
{
    if (s_seq_count == 0) return;

    s_seq_index += delta;
    if (s_seq_index < 0) s_seq_index = s_seq_count - 1;
    if (s_seq_index >= s_seq_count) s_seq_index = 0;
    show_frame();
}

/* ------------------------------ 播放控制 ------------------------------ */

static void close_player(void)
{
    s_playing = false;
    if (s_player == NULL) return;

    lvgl_port_lock(0);
    lv_obj_del(s_player);
    s_player = NULL;
    s_img = NULL;
    s_frame_label = NULL;
    s_play_label = NULL;
    lvgl_port_unlock();
}

static void play_pause_cb(lv_event_t *e)
{
    (void)e;

    if (s_seq_count == 0) return;

    s_playing = !s_playing;
    if (s_play_label != NULL) {
        lv_label_set_text(s_play_label, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    step(-1);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    step(1);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    close_player();
}

static void frame_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_playing || s_player == NULL) return;
    step(1);
}

/* ------------------------------ 列表 ------------------------------ */

static void open_dir_cb(lv_event_t *e)
{
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_row_count) return;

    if (open_sequence(s_rows[idx]) < 2) {
        fw_ui_toast("该目录没有帧序列（frame_0001.bmp/png）", 2500);
        return;
    }

    lvgl_port_lock(0);

    s_player = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_player, lv_pct(100), lv_disp_get_ver_res(NULL) - FW_STATUSBAR_H);
    lv_obj_set_pos(s_player, 0, FW_STATUSBAR_H);
    lv_obj_clear_flag(s_player, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_player, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(s_player, 4, 0);
    lv_obj_set_style_border_width(s_player, 0, 0);

    s_img = lv_img_create(s_player);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *bar = lv_obj_create(s_player);
    lv_obj_set_size(bar, lv_pct(100), 36);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const struct {
        const char *symbol;
        lv_event_cb_t cb;
    } k_btns[] = {
        { LV_SYMBOL_PREV, prev_cb },
        { LV_SYMBOL_PAUSE, play_pause_cb },
        { LV_SYMBOL_NEXT, next_cb },
        { LV_SYMBOL_CLOSE, close_cb },
    };

    for (size_t i = 0; i < sizeof(k_btns) / sizeof(k_btns[0]); i++) {
        lv_obj_t *btn = lv_btn_create(bar);
        lv_obj_set_size(btn, 50, 32);
        lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
        lv_obj_add_event_cb(btn, k_btns[i].cb, LV_EVENT_SHORT_CLICKED, NULL);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, k_btns[i].symbol);
        lv_obj_set_style_text_color(label,
                                    (i == 3) ? fw_theme_color_text_primary()
                                             : fw_theme_color_accent(), 0);
        lv_obj_center(label);

        if (i == 1) s_play_label = label;
    }

    s_frame_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_frame_label, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_frame_label, fw_theme_color_text_secondary(), 0);
    lv_obj_set_width(s_frame_label, 60);
    lv_obj_set_style_text_align(s_frame_label, LV_TEXT_ALIGN_CENTER, 0);

    show_frame();
    lvgl_port_unlock();

    s_playing = true;
}

static void scan(void)
{
    s_row_count = 0;

    static const char *bases[] = { "/sdcard", "/internal" };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
        svc_storage_iter_t it = NULL;
        if (svc_storage_iter_start(bases[b], &it) != ESP_OK) continue;

        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL) {
            if (s_row_count >= VID_ROW_MAX) break;
            if (!entry->is_dir) continue;

            char full[VID_ROW_LEN];
            int n = snprintf(full, sizeof(full), "%.40s/%.48s", bases[b], entry->name);
            if (n <= 0 || (size_t)n >= sizeof(full)) continue;

            if (open_sequence(full) < 2) continue;   /* 不是帧序列目录就跳过 */

            strlcpy(s_rows[s_row_count], full, VID_ROW_LEN);
            s_row_count++;
        }
        svc_storage_iter_end(it);
    }

    if (s_list != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_list);
        if (s_row_count == 0) {
            fw_ui_list_add(s_list, "没有帧序列目录（frame_0001.bmp/png）", NULL, NULL);
        } else {
            for (size_t i = 0; i < s_row_count; i++) {
                fw_ui_list_add(s_list, s_rows[i], open_dir_cb, (void *)(intptr_t)i);
            }
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "scan: %u sequence(s)", (unsigned)s_row_count);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *video_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_list = fw_ui_list(body, "帧序列目录");
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    lvgl_port_unlock();

    scan();

    s_timer = lv_timer_create(frame_timer_cb, 1000 / VID_FPS_DEFAULT, NULL);

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void video_on_destroy(void *ctx)
{
    (void)ctx;

    s_playing = false;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_player != NULL) {
        lv_obj_del(s_player);
        s_player = NULL;
    }
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_list = NULL;
    s_img = NULL;
    s_frame_label = NULL;
    s_play_label = NULL;
    lvgl_port_unlock();
}

static bool video_on_back(void *ctx)
{
    (void)ctx;

    if (s_player != NULL) {
        close_player();
        return true;
    }
    return false;
}

const fw_app_desc_t app_video_desc = {
    .name = "Video",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_VIDEO,
    .on_create = video_on_create,
    .on_pause = NULL,
    .on_resume = NULL,
    .on_destroy = video_on_destroy,
    .on_back = video_on_back,
};
