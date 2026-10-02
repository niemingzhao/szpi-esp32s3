/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Music（APP-MUSIC 音乐）
 *
 * 扫描 TF 卡（没有卡时退回内置 SPIFFS）根目录的 MP3 / WAV，列表点选播放；
 * 播放状态每 500 ms 从 svc_audio 读一次（见 AGENTS：svc_audio 的注册回调是单槽位，
 * 多个消费者会互相覆盖，所以这里轮询而不是抢回调）。
 * 一首放完（状态回到 IDLE）自动播下一首。
 */

#include "app_music.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.music";

#define MUSIC_MAX       16
#define MUSIC_NAME_MAX  64
#define MUSIC_PATH_MAX  300

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_now = NULL;
static lv_obj_t *s_hint = NULL;
static lv_timer_t *s_timer = NULL;

static char s_base[16] = "/sdcard";
/* 曲目名列表放堆上（MUSIC_MAX × MUSIC_NAME_MAX = 1 KB），不在 App 里放大静态数组（见 AGENTS 4.20） */
static char (*s_names)[MUSIC_NAME_MAX] = NULL;
static char s_ext_path[MUSIC_PATH_MAX];   /* 由 File App 带参数指定的曲目（可能不在列表里） */
static size_t s_count = 0;
static int s_index = -1;
static bool s_playing = false;

/* ------------------------------- 工具 ------------------------------- */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n <= e) return false;

    const char *p = name + (n - e);
    for (size_t i = 0; i < e; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ext[i]) return false;
    }
    return true;
}

static bool is_audio(const char *name)
{
    return has_ext(name, ".mp3") || has_ext(name, ".wav");
}

static void build_path(size_t idx, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", s_base, s_names[idx]);
}

static void refresh_status(void)
{
    if (s_now == NULL || s_hint == NULL) return;

    if (s_index < 0 && s_ext_path[0] == '\0') {
        lv_label_set_text(s_now, "未选择音乐");
        lv_label_set_text(s_hint, "点下面的列表播放");
        return;
    }

    if (s_index >= 0 && (size_t)s_index < s_count) {
        lv_label_set_text(s_now, s_names[s_index]);
    } else {
        const char *name = strrchr(s_ext_path, '/');
        lv_label_set_text(s_now, (name != NULL) ? name + 1 : s_ext_path);
    }

    const char *txt = "已停止";
    switch (svc_audio_get_state()) {
    case SVC_AUDIO_STATE_PLAYING:   txt = "播放中"; break;
    case SVC_AUDIO_STATE_PAUSED:    txt = "已暂停"; break;
    case SVC_AUDIO_STATE_RECORDING: txt = "录音中"; break;
    default:                        txt = "已停止"; break;
    }
    lv_label_set_text(s_hint, txt);
}

static void play_index(int idx)
{
    if (idx < 0 || (size_t)idx >= s_count) return;

    char path[MUSIC_PATH_MAX];
    build_path((size_t)idx, path, sizeof(path));

    const svc_audio_source_t src = {
        .type = SVC_AUDIO_SRC_FILE,
        .uri = path,
        .loop = false,
    };

    esp_err_t err = svc_audio_play(&src);
    if (err != ESP_OK) {
        fw_ui_toast("播放失败", 2000);
        ESP_LOGW(TAG, "play %s failed: %s", path, esp_err_to_name(err));
        return;
    }

    s_index = idx;
    s_ext_path[0] = '\0';
    s_playing = true;
    refresh_status();
}

/* File App 用 "Music?path=/sdcard/xxx.mp3" 启动时，直接播这个文件 */
static void play_external(void)
{
    const char *args = fw_app_mgr_get_args();
    if (args == NULL) return;

    const char *p = strstr(args, "path=");
    if (p == NULL) return;
    p += 5;
    if (p[0] != '/') return;

    size_t size = 0;
    if (svc_storage_exists(p, &size) != ESP_OK) {
        fw_ui_toast("文件不存在", 2000);
        return;
    }

    strlcpy(s_ext_path, p, sizeof(s_ext_path));
    s_index = -1;

    const svc_audio_source_t src = { .type = SVC_AUDIO_SRC_FILE, .uri = s_ext_path };
    if (svc_audio_play(&src) != ESP_OK) {
        fw_ui_toast("播放失败", 2000);
        s_ext_path[0] = '\0';
        return;
    }

    s_playing = true;
    refresh_status();
}

/* ------------------------------- 列表 ------------------------------- */

static void row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    play_index(idx);
}

static void scan(void)
{
    s_count = 0;
    s_index = -1;

    if (s_names == NULL) {
        if (s_list != NULL) {
            lvgl_port_lock(0);
            lv_obj_clean(s_list);
            fw_ui_list_add(s_list, "内存不足", NULL, NULL);
            lvgl_port_unlock();
        }
        return;
    }

    /* 优先 TF 卡；没有（或没插卡）就退回内置 SPIFFS 的根目录 */
    static const char *bases[] = { "/sdcard", "/internal" };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]) && s_count == 0; b++) {
        svc_storage_iter_t it = NULL;
        if (svc_storage_iter_start(bases[b], &it) != ESP_OK) continue;

        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL) {
            if (entry->is_dir || !is_audio(entry->name)) continue;
            if (s_count >= MUSIC_MAX) break;

            strlcpy(s_names[s_count], entry->name, MUSIC_NAME_MAX);
            s_count++;
        }
        svc_storage_iter_end(it);

        if (s_count > 0) strlcpy(s_base, bases[b], sizeof(s_base));
    }

    if (s_list != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_list);
        if (s_count == 0) {
            fw_ui_list_add(s_list, "没有找到 MP3 / WAV 文件", NULL, NULL);
        } else {
            for (size_t i = 0; i < s_count; i++) {
                fw_ui_list_add(s_list, s_names[i], row_cb, (void *)(intptr_t)i);
            }
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "scan %s: %u file(s)", s_base, (unsigned)s_count);
    refresh_status();
}

/* ------------------------------ 控制按钮 ------------------------------ */

static void prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;

    int idx = (s_index <= 0) ? (int)s_count - 1 : s_index - 1;
    play_index(idx);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;

    int idx = ((size_t)(s_index + 1) >= s_count) ? 0 : s_index + 1;
    play_index(idx);
}

static void play_pause_cb(lv_event_t *e)
{
    (void)e;

    if (s_count == 0) return;

    svc_audio_state_t st = svc_audio_get_state();
    if (st == SVC_AUDIO_STATE_PLAYING) {
        svc_audio_pause();
        s_playing = false;
    } else if (st == SVC_AUDIO_STATE_PAUSED) {
        svc_audio_resume();
        s_playing = true;
    } else if (s_ext_path[0] != '\0') {
        play_external();
        return;
    } else {
        play_index(s_index >= 0 ? s_index : 0);
        return;
    }
    refresh_status();
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    svc_audio_stop();
    s_playing = false;
    refresh_status();
}

static void volume_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    svc_audio_set_volume((uint8_t)lv_slider_get_value(slider));
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;

    svc_audio_state_t st = svc_audio_get_state();

    /* 播完自动下一首 */
    if (s_playing && st == SVC_AUDIO_STATE_IDLE && s_count > 0) {
        s_playing = false;
        next_cb(NULL);
        return;
    }
    refresh_status();
}

/* ------------------------------ 生命周期 ------------------------------ */

static lv_obj_t *add_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 56, 44);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_accent(), 0);
    lv_obj_center(label);
    return btn;
}

static void *music_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_names == NULL) {
        s_names = malloc((size_t)MUSIC_MAX * MUSIC_NAME_MAX);
    }

    s_now = lv_label_create(body);
    lv_label_set_text(s_now, "未选择音乐");
    lv_obj_set_style_text_font(s_now, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_now, fw_theme_color_text_primary(), 0);

    s_hint = lv_label_create(body);
    lv_label_set_text(s_hint, "点下面的列表播放");
    lv_obj_set_style_text_font(s_hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_hint, fw_theme_color_text_secondary(), 0);

    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_size(row, lv_pct(100), 48);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_btn(row, LV_SYMBOL_PREV, prev_cb);
    add_btn(row, LV_SYMBOL_PLAY, play_pause_cb);
    add_btn(row, LV_SYMBOL_NEXT, next_cb);
    add_btn(row, LV_SYMBOL_STOP, stop_cb);

    fw_ui_slider_row(body, &icon_ui_speaker, "音量", 0, 100, svc_audio_get_volume(),
                     volume_cb, NULL);

    s_list = fw_ui_list(body, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    scan();
    play_external();          /* File App 带 path= 启动时直接播 */

    s_timer = lv_timer_create(timer_cb, 500, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void music_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void music_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh_status();
}

static void music_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_list = NULL;
    s_now = NULL;
    s_hint = NULL;
    lvgl_port_unlock();

    free(s_names);
    s_names = NULL;
    s_count = 0;
}

const fw_app_desc_t app_music_desc = {
    .name = "Music",
    .title = "音乐",
    .icon_64 = &icon_home_music,
    .symbol = LV_SYMBOL_AUDIO,
    .on_create = music_on_create,
    .on_pause = music_on_pause,
    .on_resume = music_on_resume,
    .on_destroy = music_on_destroy,
};
