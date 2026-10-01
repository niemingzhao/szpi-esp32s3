/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Recorder（APP-RECORDER 录音机）
 *
 * 录制 16 kHz / 16-bit / 立体声 WAV（svc_audio 里 44 字节标准 WAV 头 + 流式写文件），
 * 文件名 REC_<年月日>_<时分秒>.wav（时间没同步时用开机秒数），最长 300 s。
 * 列表点一下回放，长按删除（二次确认）。
 */

#include "app_recorder.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.recorder";

#define REC_MAX       12
#define REC_NAME_MAX  64
#define REC_PATH_MAX  300
#define REC_MAX_SEC   300

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_state = NULL;
static lv_obj_t *s_elapsed = NULL;
static lv_obj_t *s_record_btn = NULL;
static lv_obj_t *s_record_label = NULL;
static lv_timer_t *s_timer = NULL;

static char s_base[16] = "/sdcard";
/* 录音文件名列表放堆上（REC_MAX × REC_NAME_MAX = 768 B），不在 App 里放大静态数组（见 AGENTS 4.20） */
static char (*s_names)[REC_NAME_MAX] = NULL;
static size_t s_count = 0;
static bool s_recording = false;
static uint32_t s_start_s = 0;

/* 前置声明：scan() 与列表回调互相调用（列表在 scan 里重建，删除回调又要刷新列表） */
static void scan(void);
static void row_play_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);

/* ------------------------------- 工具 ------------------------------- */

static bool is_recording_file(const char *name)
{
    return (strncmp(name, "REC_", 4) == 0) && (strstr(name, ".wav") != NULL);
}

static void build_path(size_t idx, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", s_base, s_names[idx]);
}

static void refresh_state(void)
{
    if (s_state == NULL) return;

    svc_audio_state_t st = svc_audio_get_state();
    s_recording = (st == SVC_AUDIO_STATE_RECORDING);

    if (s_recording) {
        lv_label_set_text(s_state, "录音中");
        lv_obj_set_style_text_color(s_state, fw_theme_color_error(), 0);
        if (s_record_label != NULL) lv_label_set_text(s_record_label, "停止");
    } else {
        uint32_t elapsed = 0;
        if (s_start_s > 0) {
            uint32_t now = (uint32_t)svc_time_now();
            elapsed = (now > s_start_s) ? (now - s_start_s) : 0;
        }
        if (st == SVC_AUDIO_STATE_PLAYING) {
            lv_label_set_text(s_state, "回放中");
            lv_obj_set_style_text_color(s_state, fw_theme_color_success(), 0);
        } else {
            lv_label_set_text(s_state, "就绪");
            lv_obj_set_style_text_color(s_state, fw_theme_color_text_secondary(), 0);
        }
        if (s_record_label != NULL) lv_label_set_text(s_record_label, "开始录音");

        char buf[32];
        snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(elapsed / 60), (unsigned)(elapsed % 60));
        if (s_elapsed != NULL) lv_label_set_text(s_elapsed, buf);
    }
}

static void scan(void)
{
    s_count = 0;

    if (s_names == NULL) {
        if (s_list != NULL) {
            lvgl_port_lock(0);
            lv_obj_clean(s_list);
            fw_ui_list_add(s_list, "内存不足", NULL, NULL);
            lvgl_port_unlock();
        }
        return;
    }

    static const char *bases[] = { "/sdcard", "/internal" };
    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]) && s_count == 0; b++) {
        svc_storage_iter_t it = NULL;
        if (svc_storage_iter_start(bases[b], &it) != ESP_OK) continue;

        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL) {
            if (entry->is_dir || !is_recording_file(entry->name)) continue;
            if (s_count >= REC_MAX) break;

            strlcpy(s_names[s_count], entry->name, REC_NAME_MAX);
            s_count++;
        }
        svc_storage_iter_end(it);
        if (s_count > 0) strlcpy(s_base, bases[b], sizeof(s_base));
    }

    if (s_list != NULL) {
        lvgl_port_lock(0);
        lv_obj_clean(s_list);
        if (s_count == 0) {
            fw_ui_list_add(s_list, "还没有录音", NULL, NULL);
        } else {
            for (size_t i = 0; i < s_count; i++) {
                lv_obj_t *row = fw_ui_list_add(s_list, s_names[i], row_play_cb,
                                               (void *)(intptr_t)i);
                if (row != NULL) {
                    lv_obj_add_event_cb(row, row_long_cb, LV_EVENT_LONG_PRESSED,
                                        (void *)(intptr_t)i);
                }
            }
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "scan %s: %u recording(s)", s_base, (unsigned)s_count);
}

/* ------------------------------ 交互 ------------------------------ */

static void make_record_name(char *buf, size_t len)
{
    if (svc_time_is_synced()) {
        char ts[32];
        svc_time_format(svc_time_now(), "%Y%m%d_%H%M%S", ts, sizeof(ts));
        snprintf(buf, len, "%s/REC_%s.wav", s_base, ts);
    } else {
        snprintf(buf, len, "%s/REC_%05u.wav", s_base, (unsigned)svc_time_now() % 100000u);
    }
}

static void record_cb(lv_event_t *e)
{
    (void)e;

    if (svc_audio_get_state() == SVC_AUDIO_STATE_RECORDING) {
        svc_audio_record_stop();
        fw_ui_toast("录音已保存", 2000);
        refresh_state();
        scan();
        return;
    }

    char path[REC_PATH_MAX];
    make_record_name(path, sizeof(path));

    if (svc_audio_record_start(path, REC_MAX_SEC) != ESP_OK) {
        fw_ui_toast("录音启动失败", 2000);
        return;
    }

    s_start_s = (uint32_t)svc_time_now();
    refresh_state();
    ESP_LOGI(TAG, "recording to %s", path);
}

/* 长按列表项：删除（二次确认） */
static void row_delete_cb(fw_dialog_btn_t btn, void *user)
{
    if (btn != FW_DIALOG_BTN_OK) return;

    size_t idx = (size_t)(intptr_t)user;
    if (idx >= s_count) return;

    char path[REC_PATH_MAX];
    build_path(idx, path, sizeof(path));

    if (svc_storage_remove(path) == ESP_OK) {
        fw_ui_toast("已删除", 1500);
    } else {
        fw_ui_toast("删除失败", 1500);
    }
    scan();
}

static void row_long_cb(lv_event_t *e)
{
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_count) return;

    fw_ui_dialog(NULL, "删除录音", s_names[idx], FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 row_delete_cb, (void *)(intptr_t)idx);
}

static void row_play_cb(lv_event_t *e)
{
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_count) return;

    char path[REC_PATH_MAX];
    build_path(idx, path, sizeof(path));

    const svc_audio_source_t src = { .type = SVC_AUDIO_SRC_FILE, .uri = path };
    if (svc_audio_play(&src) != ESP_OK) {
        fw_ui_toast("回放失败", 2000);
        return;
    }
    refresh_state();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_state();
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *recorder_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    if (s_names == NULL) {
        s_names = malloc((size_t)REC_MAX * REC_NAME_MAX);
    }

    s_state = lv_label_create(body);
    lv_label_set_text(s_state, "就绪");
    lv_obj_set_style_text_font(s_state, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_state, fw_theme_color_text_secondary(), 0);

    s_elapsed = lv_label_create(body);
    lv_label_set_text(s_elapsed, "0:00");
    lv_obj_set_style_text_font(s_elapsed, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_elapsed, fw_theme_color_text_primary(), 0);

    s_record_btn = fw_ui_row_btn(body, LV_SYMBOL_AUDIO, "开始录音", record_cb, NULL);
    s_record_label = lv_obj_get_child(s_record_btn, 1);   /* 见 fw_ui_row_btn 的子对象顺序 */

    s_list = fw_ui_list(body, "录音文件");
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    scan();

    s_timer = lv_timer_create(timer_cb, 1000, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void recorder_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void recorder_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    refresh_state();
}

static void recorder_on_destroy(void *ctx)
{
    (void)ctx;

    if (svc_audio_get_state() == SVC_AUDIO_STATE_RECORDING) {
        svc_audio_record_stop();
    }

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
    s_state = NULL;
    s_elapsed = NULL;
    s_record_btn = NULL;
    s_record_label = NULL;
    lvgl_port_unlock();

    free(s_names);
    s_names = NULL;
    s_count = 0;
}

const fw_app_desc_t app_recorder_desc = {
    .name = "Recorder",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_AUDIO,
    .on_create = recorder_on_create,
    .on_pause = recorder_on_pause,
    .on_resume = recorder_on_resume,
    .on_destroy = recorder_on_destroy,
};
