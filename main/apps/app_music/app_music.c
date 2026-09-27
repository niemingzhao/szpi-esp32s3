/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Music（音乐）
 *
 * 扫 TF 卡与内置存储里的 MP3 / WAV（含子目录，最多 MUSIC_MAX 首），列表点选播放。
 * 顶行显示当前曲目与状态，底部是上一首 / 播放暂停 / 下一首；列表里正在放的那首标主色。
 * 一首放完（状态从"播放中"回到空闲）自动下一首，到最后绕回第一首。
 *
 * 播放状态每 500 ms 从 svc_audio 读一次：播放结束事件不带来源，注册回调又是单槽位
 * （多个消费者会互相覆盖），所以这里轮询；离开前台时把定时器停掉，不刷后台界面。
 *
 * 曲目完整路径放堆上（MUSIC_MAX × MUSIC_PATH_MAX = 4.8 KB），不在 App 里放大静态数组。
 */

#include "app_music.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.music";

#define MUSIC_INIT       32     /* 曲目列表起始容量（按需成倍扩） */
#define MUSIC_MAX        512    /* 上限：再多列表也放不下，防御性拦截 */
#define MUSIC_PATH_MAX   200    /* 路径缓冲，与文件管理的 PATH_MAX_LEN 一致 */
#define MUSIC_SCAN_DEPTH 3

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_now = NULL;          /* 顶行：当前曲目名 */
static lv_obj_t *s_state = NULL;        /* 顶行：播放状态 */
static lv_obj_t *s_play_img = NULL;     /* 播放 / 暂停按钮里的图标 */
static lv_obj_t *s_list = NULL;
static lv_obj_t **s_items = NULL;       /* 列表项（跟 s_paths 同容量），用来标"正在播放" */
static lv_timer_t *s_timer = NULL;

static char (*s_paths)[MUSIC_PATH_MAX] = NULL;
static size_t s_paths_cap = 0;
static size_t s_count = 0;
static char s_path[MUSIC_PATH_MAX] = "";   /* 当前曲目的完整路径（不在列表里也能播） */
static int s_mark_idx = -1;
static bool s_was_playing = false;         /* 上一次看到的状态是"播放中" */
static fw_ui_stage_t *s_stage = NULL;      /* 列表分段构建 */

/* 前置声明 */
static void scan(void);
static void refresh_status(void);
static void play_index(size_t idx);
static void play_external(void);
static void row_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);
static void prev_cb(lv_event_t *e);
static void play_cb(lv_event_t *e);
static void next_cb(lv_event_t *e);

/* ------------------------------- 小工具 ------------------------------- */

/* 保证曲目列表至少能放 need 项；不够就成倍扩（上限 MUSIC_MAX）。
 * 路径与列表项两个数组一起扩，容量始终一致 */
static bool music_reserve(size_t need)
{
    if (need <= s_paths_cap) return true;
    if (need > MUSIC_MAX) return false;

    size_t cap = (s_paths_cap > 0) ? s_paths_cap : MUSIC_INIT;
    while (cap < need) cap *= 2;
    if (cap > MUSIC_MAX) cap = MUSIC_MAX;

    void *np = realloc(s_paths, cap * MUSIC_PATH_MAX);
    if (np == NULL) return false;
    s_paths = np;

    void *ni = realloc(s_items, cap * sizeof(*s_items));
    if (ni == NULL) return false;       /* 路径数组已扩好，下次调用会接着把这项补齐 */
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

static bool is_audio(const char *name)
{
    return has_ext(name, ".mp3") || has_ext(name, ".wav");
}

/* 列表里显示的名字：去掉存储根前缀（子目录里的曲子带一级目录，便于区分） */
static const char *rel_name(const char *path)
{
    if (strncmp(path, "/sdcard/", 8) == 0) return path + 8;
    if (strncmp(path, "/internal/", 10) == 0) return path + 10;
    return path;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

static const char *state_text(svc_audio_state_t st)
{
    switch (st) {
    case SVC_AUDIO_STATE_PLAYING:   return "播放中";
    case SVC_AUDIO_STATE_PAUSED:    return "已暂停";
    case SVC_AUDIO_STATE_RECORDING: return "录音中";
    default:                        return "已停止";
    }
}

static int index_of_current(void)
{
    if (s_path[0] == '\0') return -1;

    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_paths[i], s_path) == 0) return (int)i;
    }
    return -1;
}

/* 顶行与列表标记按真实状态重刷（进前台、换曲、定时器都调） */
static void refresh_status(void)
{
    if (s_now == NULL || s_state == NULL) return;

    const svc_audio_state_t st = svc_audio_get_state();

    lvgl_port_lock(0);

    if (s_path[0] == '\0') {
        lv_label_set_text(s_now, "未选择音乐");
        lv_label_set_text(s_state, "");
    } else {
        lv_label_set_text(s_now, path_basename(s_path));
        lv_label_set_text(s_state, state_text(st));
    }

    if (s_play_img != NULL) {
        lv_image_set_src(s_play_img, (st == SVC_AUDIO_STATE_PLAYING) ? &icon_ui_pause
                                                                     : &icon_ui_play);
    }

    const int idx = index_of_current();
    /* 分段构建时列表可能还没建到这一项：s_items[idx] 为 NULL 就先不动标记 */
    if (idx != s_mark_idx && s_items != NULL && (idx < 0 || s_items[idx] != NULL)) {
        if (s_mark_idx >= 0 && (size_t)s_mark_idx < s_count) {
            fw_ui_list_mark(s_items[s_mark_idx], false);
        }
        if (idx >= 0) {
            fw_ui_list_mark(s_items[idx], true);
            /* 换曲才滚过去（每 500 ms 的刷新不动滚动位置，否则用户滚不动列表） */
            lv_obj_scroll_to_view(s_items[idx], LV_ANIM_OFF);
        }
        s_mark_idx = idx;
    }

    lvgl_port_unlock();
}

/* 播放一个文件路径（来自列表或文件管理）。
 * 文件已经不在了（删了 / 格式化了 / 换了卡）就清掉当前选择，返回 ESP_ERR_NOT_FOUND */
static esp_err_t play_path(const char *path)
{
    size_t size = 0;
    if (svc_storage_exists(path, &size) != ESP_OK) {
        if (strcmp(path, s_path) == 0) {
            s_path[0] = '\0';
        }
        fw_ui_toast("文件不存在", 2000);
        refresh_status();
        return ESP_ERR_NOT_FOUND;
    }

    const svc_audio_source_t src = { .type = SVC_AUDIO_SRC_FILE, .uri = path };
    if (svc_audio_play(&src) != ESP_OK) return ESP_FAIL;

    strlcpy(s_path, path, sizeof(s_path));
    refresh_status();
    return ESP_OK;
}

static void play_index(size_t idx)
{
    if (idx >= s_count) return;

    if (play_path(s_paths[idx]) == ESP_FAIL) {
        fw_ui_toast("播放失败", 2000);
        ESP_LOGW(TAG, "play %s failed", s_paths[idx]);
    }
}

/* 上一首 / 下一首（绕回）；当前曲目不在列表里就从列表头 / 尾开始 */
static void step(int delta)
{
    if (s_count == 0) return;

    int idx = index_of_current();
    if (idx < 0) {
        idx = (delta > 0) ? 0 : (int)s_count - 1;
    } else {
        idx = (idx + delta + (int)s_count) % (int)s_count;
    }
    play_index((size_t)idx);
}

/* File App 用 "Music?path=/sdcard/xxx.mp3" 启动时，直接播这个文件 */
static void play_external(void)
{
    const char *args = fw_app_mgr_get_args();
    const char *p = (args != NULL) ? strstr(args, "path=") : NULL;

    if (p == NULL) return;

    p += 5;
    if (p[0] != '/' || !is_audio(p)) return;

    if (play_path(p) == ESP_FAIL) {
        fw_ui_toast("播放失败", 2000);
        ESP_LOGW(TAG, "play %s failed", p);
    }
}

/* ------------------------------- 扫描 ------------------------------- */

#define MUSIC_BUILD_CHUNK  12   /* 分段构建：每批建几项 */

/* 长按删除：先把目标拷出来（列表重建后会失效），确认后删掉并重扫 */
static char s_del_path[MUSIC_PATH_MAX] = "";
static char s_del_name[MUSIC_PATH_MAX] = "";

static void del_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    if (svc_storage_remove(s_del_path) != ESP_OK) {
        fw_ui_toast("删除失败", 2000);
        return;
    }

    /* 删掉的正好是正在放的那首：停下来并清掉选择 */
    if (strcmp(s_del_path, s_path) == 0) {
        svc_audio_stop();
        s_path[0] = '\0';
    }

    fw_ui_toast("已删除", 1500);
    ESP_LOGI(TAG, "removed %s", s_del_path);

    scan();                                 /* 列表去掉这一首，顺便重刷状态 */
}

static void row_long_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_count) return;

    strlcpy(s_del_path, s_paths[idx], sizeof(s_del_path));
    strlcpy(s_del_name, rel_name(s_paths[idx]), sizeof(s_del_name));

    char msg[MUSIC_PATH_MAX + 64];
    snprintf(msg, sizeof(msg), "删除「%s」？删除后无法恢复。", s_del_name);
    fw_ui_dialog(NULL, "删除音乐", msg, FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 del_confirm_cb, NULL);
}

/* 分段建第 idx 项（由 fw_ui_stage_start 调，已在 LVGL 锁内） */
static void build_row(size_t idx, void *user)
{
    (void)user;
    if (s_list == NULL || s_items == NULL || idx >= s_count) return;

    s_items[idx] = fw_ui_list_add_icon(s_list, &icon_ui_file_audio, rel_name(s_paths[idx]),
                                       NULL, row_cb, (void *)(intptr_t)idx);
    if (s_items[idx] != NULL) {
        lv_obj_add_event_cb(s_items[idx], row_long_cb, LV_EVENT_LONG_PRESSED,
                            (void *)(intptr_t)idx);
    }
}

/* 全部建完：空曲库提示 + 把"正在播放"标出来（这时列表项才都在） */
static void build_done(void *user)
{
    (void)user;

    lvgl_port_lock(0);
    if (s_list != NULL && s_count == 0) {
        fw_ui_list_hint(s_list, "没有找到 MP3 / WAV 文件");
    }
    lvgl_port_unlock();

    refresh_status();
}

/* 递归扫一个目录（层数与曲目数都有上限） */
static void scan_dir(const char *dir, int depth)
{
    if (depth > MUSIC_SCAN_DEPTH || s_count >= MUSIC_MAX) return;

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) return;

    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL) {
        if (s_count >= MUSIC_MAX) break;

        if (entry->is_dir) {
            if (entry->name[0] == '.') continue;    /* 隐藏目录 / 系统目录 */

            char sub[MUSIC_PATH_MAX];
            if (snprintf(sub, sizeof(sub), "%s/%s", dir, entry->name) >= (int)sizeof(sub)) continue;
            scan_dir(sub, depth + 1);
            continue;
        }

        if (!is_audio(entry->name)) continue;
        if (!music_reserve(s_count + 1)) break;     /* 到上限或内存不足：停在这里 */

        char full[MUSIC_PATH_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, entry->name) >= (int)sizeof(full)) continue;
        strlcpy(s_paths[s_count], full, MUSIC_PATH_MAX);
        s_count++;
    }

    svc_storage_iter_end(it);
}

static void scan(void)
{
    s_count = 0;

    if (s_paths != NULL && s_items != NULL) {
        scan_dir("/sdcard", 1);
        scan_dir("/internal", 1);
    }

    /* 当前曲目可能已经不在了（删了 / 格式化 / 换卡）：清掉，顶行别留着旧歌名 */
    if (s_path[0] != '\0') {
        size_t size = 0;
        if (svc_storage_exists(s_path, &size) != ESP_OK) {
            s_path[0] = '\0';
        }
    }

    if (s_list != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_list);
        if (s_items != NULL) {
            for (size_t i = 0; i < s_paths_cap; i++) s_items[i] = NULL;
        }
        s_mark_idx = -1;

        /* 曲库可能几百首，分段建；建完在 build_done 里补空提示与"正在播放"标记 */
        fw_ui_stage_start(s_stage, s_count, MUSIC_BUILD_CHUNK, build_row, build_done, NULL);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "scan: %u file(s)", (unsigned)s_count);
    refresh_status();
}

/* ------------------------------ 控制按钮 ------------------------------ */

static void row_cb(lv_event_t *e)
{
    play_index((size_t)(intptr_t)lv_event_get_user_data(e));
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

static void play_cb(lv_event_t *e)
{
    (void)e;

    const svc_audio_state_t st = svc_audio_get_state();

    if (st == SVC_AUDIO_STATE_PLAYING) {
        svc_audio_pause();
        s_was_playing = false;
    } else if (st == SVC_AUDIO_STATE_PAUSED) {
        svc_audio_resume();
        s_was_playing = true;
    } else if (s_path[0] != '\0') {
        if (play_path(s_path) == ESP_FAIL) fw_ui_toast("播放失败", 2000);
        return;
    } else if (s_count > 0) {
        play_index(0);
        return;
    } else {
        fw_ui_toast("没有可播放的音乐", 1800);
        return;
    }

    refresh_status();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;

    const svc_audio_state_t st = svc_audio_get_state();

    if (st == SVC_AUDIO_STATE_PLAYING) {
        s_was_playing = true;
    } else if (st == SVC_AUDIO_STATE_IDLE) {
        /* 播完（从"播放中"回到空闲）自动下一首；暂停 / 停止不会走到这里 */
        if (s_was_playing && s_count > 0) {
            s_was_playing = false;
            step(1);
            return;
        }
        s_was_playing = false;
    }

    refresh_status();
}

/* ------------------------------ 生命周期 ------------------------------ */

/* 播放控制按钮：返回按钮，图标对象用 icon_out 带出来（播放 / 暂停要换图标） */
static lv_obj_t *transport_btn(lv_obj_t *parent, const lv_image_dsc_t *icon, int32_t w,
                               lv_obj_t **icon_out, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 44);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *img = fw_ui_icon(btn, icon, fw_theme_color_accent());
    lv_obj_center(img);

    if (icon_out != NULL) *icon_out = img;
    return btn;
}

static void *music_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_paths == NULL || s_items == NULL) {
        music_reserve(MUSIC_INIT);
    }
    if (s_paths == NULL || s_items == NULL) {
        fw_ui_toast("内存不足", 2000);
    }

    if (s_stage == NULL) {
        s_stage = fw_ui_stage_create();
    }

    /* 顶行：当前曲目 + 状态 */
    lv_obj_t *head = lv_obj_create(body);
    lv_obj_set_size(head, lv_pct(100), 28);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    fw_ui_icon(head, &icon_ui_file_audio, fw_theme_color_accent());

    s_now = lv_label_create(head);
    lv_obj_set_flex_grow(s_now, 1);
    lv_label_set_long_mode(s_now, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_now, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_now, fw_theme_color_text_primary(), 0);

    s_state = lv_label_create(head);
    lv_obj_set_style_text_font(s_state, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state, fw_theme_color_text_secondary(), 0);

    /* 列表（余下的高度都给它） */
    s_list = fw_ui_list(body, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    /* 底部：上一首 / 播放暂停 / 下一首 */
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), 44);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    transport_btn(bar, &icon_ui_left2, 64, NULL, prev_cb);
    transport_btn(bar, &icon_ui_play, 72, &s_play_img, play_cb);
    transport_btn(bar, &icon_ui_right2, 64, NULL, next_cb);

    lvgl_port_unlock();

    /* 界面建好后填内容：扫库，再处理文件管理传来的 path= */
    scan();
    play_external();

    s_timer = lv_timer_create(timer_cb, 500, NULL);

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* 首次创建后与"从返回栈回来"（on_resume）：桌面用主页键回来时 on_start 也会跟着来 */
static void music_on_start(void *ctx)
{
    (void)ctx;

    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh_status();
}

static void music_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);

    /* 离开前台就不再自动续播：这段时间里进度看不到，可能是别的 App（声音试听 / 脚本）
     * 把音频通道占了，回到前台时状态也是空闲 —— 那不该被当成"这首放完了" */
    s_was_playing = false;

    /* 真的离开 App（回桌面 / 返回上一级）就把播放停掉：换主题是在前台原地重建、
     * is_foreground 仍为真，那种情况不该打断正在放的音乐 */
    if (!fw_app_mgr_is_foreground("Music")) {
        svc_audio_stop();
    }
}

/* 重新进前台：重扫曲库，并播放文件管理刚点的那首（参数只在这里处理，
 * on_create 管首次创建）—— 这样反复点同一首歌每次都会重新播 */
static void music_on_resume(void *ctx)
{
    (void)ctx;

    if (s_timer != NULL) lv_timer_resume(s_timer);
    scan();
    play_external();
}

static void music_on_destroy(void *ctx)
{
    (void)ctx;

    /* 先停分段构建：回调会用到下面要删的列表 */
    fw_ui_stage_stop(s_stage);

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_now = NULL;
    s_state = NULL;
    s_play_img = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    free(s_paths);
    s_paths = NULL;
    free(s_items);
    s_items = NULL;
    s_paths_cap = 0;
    s_count = 0;
    s_mark_idx = -1;
    fw_ui_stage_destroy(s_stage);
    s_stage = NULL;
}

const fw_app_desc_t app_music_desc = {
    .name = "Music",
    .title = "音乐",
    .icon = &icon_home_music,
    .on_create = music_on_create,
    .on_start = music_on_start,
    .on_pause = music_on_pause,
    .on_resume = music_on_resume,
    .on_destroy = music_on_destroy,
};
