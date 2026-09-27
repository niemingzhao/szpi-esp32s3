/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Recorder（录音机）
 *
 * 录 16 kHz / 16-bit 立体声 WAV（svc_audio 写 44 字节标准头再流式追加），单段最长 5 分钟，
 * 文件名 REC_<年月日>_<时分秒>.wav（时间没同步时退回开机秒数）。头部行是时长 + 状态 +
 * 开始/停止按钮，下面列出两处存储里的录音：点一下回放，长按删除（二次确认）。
 *
 * 服务不提供录音进度，时长由每秒轮询 svc_audio 状态、配合开始时刻算出来；表只在真的
 * "录音中"时往前走，服务没起来或者中途停了会把时长冻住并提示一声。离开前台把定时器
 * 停掉不刷后台界面；重新进前台可能是 on_start（从桌面进来）也可能是 on_resume（从返回栈
 * 回来），两个都挂同一个处理函数，用 s_foreground 去重。
 *
 * 录音固定写在启动时探测到的可写存储根目录（TF 卡优先），列表则两处存储都扫。
 * 路径 / 大小 / 列表项三个数组放堆上（REC_MAX × REC_PATH_MAX = 51 KB），
 * 不在 App 里放大静态数组。
 */

#include "app_recorder.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.recorder";

#define REC_INIT        16      /* 列表起始容量（按需成倍扩） */
#define REC_MAX         256     /* 上限：再多列表也放不下，防御性拦截 */
#define REC_PATH_MAX    200     /* 路径缓冲，与文件管理的 PATH_MAX_LEN 一致 */
#define REC_SCAN_DEPTH  3       /* 目录递归层数 */
#define REC_MAX_SEC     300     /* 单段最长 5 分钟 */
#define REC_BUILD_CHUNK 12      /* 分段构建：每批建几项 */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_elapsed = NULL;      /* 头部行：时长 */
static lv_obj_t *s_state = NULL;        /* 头部行：状态文字 */
static lv_obj_t *s_rec_img = NULL;      /* 录音按钮里的图标（开始 / 停止） */
static lv_obj_t *s_list = NULL;
static lv_obj_t **s_items = NULL;       /* 列表项，跟 s_paths 同容量，用来标"正在回放" */
static lv_timer_t *s_timer = NULL;
static fw_ui_stage_t *s_stage = NULL;   /* 列表分段构建 */

static char (*s_paths)[REC_PATH_MAX] = NULL;
static size_t *s_sizes = NULL;
static size_t s_paths_cap = 0;
static size_t s_count = 0;

static char s_save_base[16] = "/sdcard"; /* 录音写在哪儿（启动时探测） */
static char s_play_path[REC_PATH_MAX] = ""; /* 本 App 起播的文件：删它之前要先停 */
static uint32_t s_start_s = 0;          /* 本次录音开始的时刻（算时长用） */
static uint32_t s_elapsed_s = 0;        /* 界面上显示的时长（不在录就冻结不动） */
static int s_mark_idx = -1;
static bool s_foreground = false;
static bool s_was_recording = false;    /* 按过开始、还在等 / 正在录 */
static bool s_starting = false;         /* 已发出开始命令、服务还没切到"录音中" */
static bool s_rec_seen = false;         /* 这次录音真的进过"录音中" */
static uint32_t s_rec_wait = 0;         /* 等状态变"录音中"的秒数 */

/* 前置声明 */
static void scan(void);
static void refresh_state(void);
static void row_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);

/* ------------------------------- 小工具 ------------------------------- */

/* 路径 / 大小 / 列表项三个数组一起扩，容量始终一致 */
static bool rec_reserve(size_t need)
{
    if (need <= s_paths_cap) return true;
    if (need > REC_MAX) return false;

    size_t cap = (s_paths_cap > 0) ? s_paths_cap : REC_INIT;
    while (cap < need) cap *= 2;
    if (cap > REC_MAX) cap = REC_MAX;

    void *np = realloc(s_paths, cap * REC_PATH_MAX);
    if (np == NULL) return false;
    s_paths = np;

    void *ns = realloc(s_sizes, cap * sizeof(*s_sizes));
    if (ns == NULL) return false;       /* 下次调用会接着把这一项补齐 */
    s_sizes = ns;

    void *ni = realloc(s_items, cap * sizeof(*s_items));
    if (ni == NULL) return false;
    s_items = ni;

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

static bool is_recording_file(const char *name)
{
    return (strncmp(name, "REC_", 4) == 0) && has_ext(name, ".wav");
}

/* 列表里显示的名字：去掉存储根前缀（子目录里的带一级目录，便于区分） */
static const char *rel_name(const char *path)
{
    if (strncmp(path, "/sdcard/", 8) == 0) return path + 8;
    if (strncmp(path, "/internal/", 10) == 0) return path + 10;
    return path;
}

static void human_size(size_t bytes, char *out, size_t len)
{
    if (bytes < 1024) {
        snprintf(out, len, "%u B", (unsigned)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, len, "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

static const char *state_text(svc_audio_state_t st)
{
    switch (st) {
    case SVC_AUDIO_STATE_RECORDING: return "录音中";
    case SVC_AUDIO_STATE_PLAYING:   return "回放中";
    case SVC_AUDIO_STATE_PAUSED:    return "已暂停";
    default:                        return "就绪";
    }
}

static int index_of_current(void)
{
    if (s_play_path[0] == '\0') return -1;

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_paths[i], s_play_path) == 0) return (int)i;
    }
    return -1;
}

/* 头部行按真实状态重刷（进前台、开始 / 停止、每秒轮询都调） */
static void refresh_state(void)
{
    if (s_elapsed == NULL || s_state == NULL) return;

    const svc_audio_state_t st = svc_audio_get_state();
    const bool live = (st == SVC_AUDIO_STATE_RECORDING);
    /* 服务是异步起来的：命令发出后先按"启动中"显示，不然会有一秒看着像没按一样 */
    const bool recording = live || s_starting;

    /* 时长只有真的在录时才往前走；停了就冻在最后一次的值上，不再自己跳 */
    if (recording) {
        const uint32_t now = (uint32_t)svc_time_now();
        s_elapsed_s = (s_start_s > 0 && now > s_start_s) ? (now - s_start_s) : 0;
    }

    char time[16];
    snprintf(time, sizeof(time), "%u:%02u", (unsigned)(s_elapsed_s / 60), (unsigned)(s_elapsed_s % 60));

    lvgl_port_lock(0);

    lv_label_set_text(s_elapsed, time);
    lv_label_set_text(s_state, (s_starting && !live) ? "启动中…" : state_text(st));

    const lv_color_t state_color = recording ? fw_theme_color_error()
                                   : (st == SVC_AUDIO_STATE_PLAYING ? fw_theme_color_success()
                                                                    : fw_theme_color_text_secondary());
    lv_obj_set_style_text_color(s_state, state_color, 0);

    if (s_rec_img != NULL) {
        lv_image_set_src(s_rec_img, recording ? &icon_ui_stop : &icon_ui_mic);
        lv_obj_set_style_image_recolor(s_rec_img,
                                       recording ? fw_theme_color_error()
                                                 : fw_theme_color_accent(), 0);
    }

    /* 列表里正在回放的那一项标主色（分段构建还没建到就先不动） */
    const int idx = (st == SVC_AUDIO_STATE_PLAYING || st == SVC_AUDIO_STATE_PAUSED)
                        ? index_of_current()
                        : -1;
    if (idx != s_mark_idx && (idx < 0 || (s_items != NULL && (size_t)idx < s_paths_cap &&
                                          s_items[idx] != NULL))) {
        if (s_mark_idx >= 0 && s_items != NULL && (size_t)s_mark_idx < s_paths_cap) {
            fw_ui_list_mark(s_items[s_mark_idx], false);
        }
        if (idx >= 0) fw_ui_list_mark(s_items[idx], true);
        s_mark_idx = idx;
    }

    lvgl_port_unlock();
}

/* ------------------------------- 扫描 ------------------------------- */

/* 分段建第 idx 项（由 fw_ui_stage_start 调，已在 LVGL 锁内） */
static void build_row(size_t idx, void *user)
{
    (void)user;
    if (s_list == NULL || s_items == NULL || idx >= s_count) return;

    char sizetxt[16];
    human_size(s_sizes[idx], sizetxt, sizeof(sizetxt));

    s_items[idx] = fw_ui_list_add_icon(s_list, &icon_ui_file_audio, rel_name(s_paths[idx]),
                                       sizetxt, row_cb, (void *)(intptr_t)idx);
    if (s_items[idx] != NULL) {
        lv_obj_add_event_cb(s_items[idx], row_long_cb, LV_EVENT_LONG_PRESSED,
                            (void *)(intptr_t)idx);
    }
}

/* 全部建完：空录音时给提示，这时列表项才都在，顺便标出正在回放的那条 */
static void build_done(void *user)
{
    (void)user;

    lvgl_port_lock(0);
    if (s_list != NULL && s_count == 0) {
        fw_ui_list_hint(s_list, "还没有录音，点右上角开始（单段最长 5 分钟）");
    }
    lvgl_port_unlock();

    refresh_state();
}

static void scan_dir(const char *dir, int depth)
{
    if (depth > REC_SCAN_DEPTH || s_count >= REC_MAX) return;

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) return;

    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL) {
        if (s_count >= REC_MAX) break;

        if (entry->is_dir) {
            if (entry->name[0] == '.') continue;    /* 隐藏目录 / 系统目录 */

            char sub[REC_PATH_MAX];
            if (snprintf(sub, sizeof(sub), "%s/%s", dir, entry->name) >= (int)sizeof(sub)) continue;
            scan_dir(sub, depth + 1);
            continue;
        }

        if (!is_recording_file(entry->name)) continue;
        if (!rec_reserve(s_count + 1)) break;       /* 到上限或内存不足：停在这里 */

        char full[REC_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, entry->name) >= (int)sizeof(full)) continue;
        strlcpy(s_paths[s_count], full, REC_PATH_MAX);
        s_sizes[s_count] = entry->size;
        s_count++;
    }

    svc_storage_iter_end(it);
}

/* 探测录音该写哪儿：TF 卡优先，没插就写内置存储 */
static void pick_save_base(void)
{
    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start("/sdcard", &it) == ESP_OK) {
        svc_storage_iter_end(it);
        strlcpy(s_save_base, "/sdcard", sizeof(s_save_base));
    } else {
        strlcpy(s_save_base, "/internal", sizeof(s_save_base));
    }
}

static void scan(void)
{
    s_count = 0;

    pick_save_base();

    if (s_paths != NULL && s_sizes != NULL && s_items != NULL) {
        scan_dir("/sdcard", 1);
        scan_dir("/internal", 1);
    }

    /* 正在回放的文件可能已经不在了（删了 / 换了卡）：清掉，别留着旧标记 */
    if (s_play_path[0] != '\0') {
        size_t size = 0;
        if (svc_storage_exists(s_play_path, &size) != ESP_OK) {
            s_play_path[0] = '\0';
        }
    }

    if (s_list != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_list);
        if (s_items != NULL) {
            for (size_t i = 0; i < s_paths_cap; i++) s_items[i] = NULL;
        }
        s_mark_idx = -1;

        /* 分段建；建完在 build_done 里补空提示与"正在回放"标记 */
        fw_ui_stage_start(s_stage, s_count, REC_BUILD_CHUNK, build_row, build_done, NULL);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "scan %s: %u recording(s)", s_save_base, (unsigned)s_count);
    refresh_state();
}

/* ------------------------------- 操作 ------------------------------- */

static void make_record_name(char *buf, size_t len)
{
    if (svc_time_is_synced()) {
        char ts[32];
        svc_time_format(svc_time_now(), "%Y%m%d_%H%M%S", ts, sizeof(ts));
        snprintf(buf, len, "%s/REC_%s.wav", s_save_base, ts);
    } else {
        snprintf(buf, len, "%s/REC_%05u.wav", s_save_base, (unsigned)svc_time_now() % 100000u);
    }
}

static void record_cb(lv_event_t *e)
{
    (void)e;

    /* 上一按还没被服务确认：忽略这次，别把同一个开始命令下两遍 */
    if (s_starting) return;

    if (svc_audio_get_state() == SVC_AUDIO_STATE_RECORDING) {
        svc_audio_record_stop();
        fw_ui_toast("录音已保存", 2000);
        s_was_recording = false;
        s_rec_seen = false;
        scan();                             /* 新文件进列表 */
        return;
    }

    char path[REC_PATH_MAX];
    make_record_name(path, sizeof(path));

    if (svc_audio_record_start(path, REC_MAX_SEC) != ESP_OK) {
        fw_ui_toast("录音启动失败", 2000);
        return;
    }

    /* 服务是异步起来的（真正的状态要等音频任务取到命令），先记开始时刻并显示
     * "启动中"，后面由 timer_cb 确认；refresh_state 只在真的在录时走表 */
    s_start_s = (uint32_t)svc_time_now();
    s_elapsed_s = 0;
    s_starting = true;
    s_rec_seen = false;
    s_rec_wait = 0;
    s_was_recording = true;
    refresh_state();
    ESP_LOGI(TAG, "recording to %s", path);
}

static void row_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_count) return;

    const svc_audio_source_t src = { .type = SVC_AUDIO_SRC_FILE, .uri = s_paths[idx] };
    if (svc_audio_play(&src) != ESP_OK) {
        fw_ui_toast("回放失败", 2000);
        ESP_LOGW(TAG, "play %s failed", s_paths[idx]);
        return;
    }

    strlcpy(s_play_path, s_paths[idx], sizeof(s_play_path));
    refresh_state();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!s_was_recording) {
        refresh_state();
        return;
    }

    const svc_audio_state_t st = svc_audio_get_state();

    if (st == SVC_AUDIO_STATE_RECORDING) {
        s_starting = false;
        s_rec_seen = true;
        s_rec_wait = 0;
        refresh_state();
        return;
    }

    if (s_rec_seen) {
        /* 录过又停了：录满 5 分钟由服务自己停，或者中途出错中止 */
        const uint32_t secs = s_elapsed_s;
        s_was_recording = false;
        s_starting = false;
        s_rec_seen = false;
        scan();                             /* 新文件进列表 */
        if (secs + 2 < REC_MAX_SEC) {
            fw_ui_toast("录音中断", 2500);
            ESP_LOGW(TAG, "recording stopped early: %u s", (unsigned)secs);
        }
        return;
    }

    /* 按了开始却一直没进"录音中"：给两秒，再没有就是没起来（服务侧会打日志） */
    if (++s_rec_wait >= 2) {
        s_was_recording = false;
        s_starting = false;
        s_rec_wait = 0;
        s_elapsed_s = 0;
        fw_ui_toast("录音启动失败", 2500);
        ESP_LOGW(TAG, "record did not start");
    }

    refresh_state();                        /* "启动中"这一两秒也把表走起来 */
}

/* ------------------------------- 删除 ------------------------------- */

/* 长按：先把目标拷出来（列表重建后会失效），确认后再删 */
static char s_del_path[REC_PATH_MAX] = "";
static char s_del_name[REC_PATH_MAX] = "";

static void del_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    if (svc_storage_remove(s_del_path) != ESP_OK) {
        fw_ui_toast("删除失败", 2000);
        return;
    }

    /* 删掉的正好是正在回放的那段：先停下来，否则文件还被播放任务占着 */
    if (strcmp(s_del_path, s_play_path) == 0) {
        svc_audio_stop();
        s_play_path[0] = '\0';
    }

    fw_ui_toast("已删除", 1500);
    ESP_LOGI(TAG, "removed %s", s_del_path);

    scan();
}

static void row_long_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_count) return;

    strlcpy(s_del_path, s_paths[idx], sizeof(s_del_path));
    strlcpy(s_del_name, rel_name(s_paths[idx]), sizeof(s_del_name));

    char msg[REC_PATH_MAX + 64];
    snprintf(msg, sizeof(msg), "删除「%s」？删除后无法恢复。", s_del_name);
    fw_ui_dialog(NULL, "删除录音", msg, FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 del_confirm_cb, NULL);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *recorder_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_paths == NULL || s_sizes == NULL || s_items == NULL) {
        rec_reserve(REC_INIT);
    }
    if (s_paths == NULL || s_sizes == NULL || s_items == NULL) {
        fw_ui_toast("内存不足", 2000);
    }

    if (s_stage == NULL) {
        s_stage = fw_ui_stage_create();
    }

    s_start_s = 0;
    s_mark_idx = -1;

    /* 录音 / 回放是 svc_audio 在做，界面重建（换主题）不该打断：正在录就留着开始时刻、
     * 正在放就留着"正在回放"的路径，重建后时长与列表标记都对得上 */
    const svc_audio_state_t st = svc_audio_get_state();
    if (st != SVC_AUDIO_STATE_RECORDING) {
        s_start_s = 0;
        s_elapsed_s = 0;
    }
    if (st != SVC_AUDIO_STATE_PLAYING && st != SVC_AUDIO_STATE_PAUSED) {
        s_play_path[0] = '\0';
    }
    s_was_recording = (st == SVC_AUDIO_STATE_RECORDING);
    s_starting = false;
    s_rec_seen = s_was_recording;
    s_rec_wait = 0;

    /* 头部行：时长 + 状态 + 开始 / 停止（按钮图标随状态换，见 refresh_state） */
    lv_obj_t *head = lv_obj_create(body);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_elapsed = lv_label_create(head);
    lv_obj_set_flex_grow(s_elapsed, 1);
    lv_label_set_text(s_elapsed, "0:00");
    lv_obj_set_style_text_font(s_elapsed, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(s_elapsed, fw_theme_color_text_primary(), 0);

    s_state = lv_label_create(head);
    lv_label_set_text(s_state, "就绪");
    lv_obj_set_style_text_font(s_state, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state, fw_theme_color_text_secondary(), 0);

    lv_obj_t *btn = fw_ui_icon_btn(head, &icon_ui_mic, NULL, 36, record_cb, NULL);
    s_rec_img = lv_obj_get_child(btn, 0);

    /* 列表（余下的高度都给它） */
    s_list = fw_ui_list(body, "录音文件");
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    /* 1 s 轮询状态：先暂停，等进入前台（on_start / on_resume）再跑 */
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    if (s_timer != NULL) {
        lv_timer_pause(s_timer);
    }

    lvgl_port_unlock();

    scan();

    ESP_LOGI(TAG, "created (base=%s)", s_save_base);
    return s_root;
}

/* on_start 与 on_resume 都挂这个函数：换主题重建时后台 App 也会走 on_create，
 * 而离开前台的 App 定时器是暂停的 —— 两条进入路径都要恢复；框架在"从桌面重新进入"
 * 时两个都会调，用 s_foreground 去重，别做两遍 */
static void recorder_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);

    scan();                                 /* 期间可能录了新文件 / 删了文件 */
}

static void recorder_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);

    /* 真的离开 App（回桌面 / 返回上一级）就停止录音与回放：这个 App 是"看着它在动"
     * 的界面，离开了不该还在录 / 还在放。换主题是前台原地重建，is_foreground 仍为真，
     * 那种情况不打断（重建后时长与状态会接上） */
    if (!fw_app_mgr_is_foreground("Recorder")) {
        if (svc_audio_get_state() == SVC_AUDIO_STATE_RECORDING) {
            svc_audio_record_stop();
        } else {
            svc_audio_stop();
        }
        s_was_recording = false;
        s_starting = false;
        s_rec_seen = false;
    }
}

static void recorder_on_destroy(void *ctx)
{
    (void)ctx;

    /* 先停分段构建：回调会用到下面要删的列表 */
    fw_ui_stage_stop(s_stage);

    /* 正在录 / 正在放都不在这里停：那是 svc_audio 的活，界面重建（换主题）不该打断它。
     * 重建时 on_create 会按真实状态把时长与列表标记接上 */

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_elapsed = NULL;
    s_state = NULL;
    s_rec_img = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    free(s_paths);
    s_paths = NULL;
    free(s_sizes);
    s_sizes = NULL;
    free(s_items);
    s_items = NULL;
    s_paths_cap = 0;
    s_count = 0;
    s_mark_idx = -1;
    s_foreground = false;
    /* s_start_s / s_play_path / s_was_recording 留给 on_create 按真实状态接续 */
    fw_ui_stage_destroy(s_stage);
    s_stage = NULL;
}

const fw_app_desc_t app_recorder_desc = {
    .name = "Recorder",
    .title = "录音机",
    .icon = &icon_home_recorder,
    .on_create = recorder_on_create,
    .on_start = recorder_on_resume,
    .on_pause = recorder_on_pause,
    .on_resume = recorder_on_resume,
    .on_destroy = recorder_on_destroy,
};
