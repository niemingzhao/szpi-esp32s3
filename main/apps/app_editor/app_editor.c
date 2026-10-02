/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Editor（APP-EDITOR 文本编辑）
 *
 * 纯文本编辑：打开 TF 卡 / 内置存储里的 .txt（整块读，上限 8 KB），软键盘输入
 * （LVGL 自带键盘，只有拉丁字符；中文没有输入法，中文内容靠打开已有文件显示），
 * 保存回原文件；没有文件名时自动生成 note_<时间>.txt。
 */

#include "app_editor.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.editor";

#define ED_MAX_TEXT    8000
#define ED_FILE_MAX    176
#define ED_LIST_MAX    12
#define ED_NAME_MAX    64

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_ta = NULL;
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_name = NULL;
static lv_obj_t *s_open_page = NULL;
static lv_obj_t *s_list = NULL;

static char s_file[ED_FILE_MAX];

/* "打开"页的候选路径放堆上（ED_LIST_MAX × ED_FILE_MAX = 2 KB）。
 * 列表项的 user_data 指向这块缓冲，所以它的生命周期跟 s_open_page 走（见 AGENTS 4.20） */
static char (*s_paths)[ED_FILE_MAX] = NULL;

/* ------------------------------- 键盘 ------------------------------- */

static void keyboard_toggle(bool show)
{
    if (s_kb == NULL) return;

    lvgl_port_lock(0);
    if (show) {
        lv_obj_set_hidden(s_kb, false);
    } else {
        lv_obj_set_hidden(s_kb, true);
    }
    lvgl_port_unlock();
}

static void kb_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_READY:
    case LV_EVENT_CANCEL:
        keyboard_toggle(false);
        break;
    default:
        break;
    }
}

static void ta_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        keyboard_toggle(true);
    }
}

/* ------------------------------- 文件 ------------------------------- */

static void update_name(void)
{
    if (s_name == NULL) return;

    const char *shown = (s_file[0] != '\0') ? s_file : "未命名";
    lv_label_set_text(s_name, shown);
}

static void make_default_name(char *buf, size_t len)
{
    if (svc_time_is_synced()) {
        char ts[32];
        svc_time_format(svc_time_now(), "%Y%m%d_%H%M%S", ts, sizeof(ts));
        snprintf(buf, len, "/sdcard/note_%s.txt", ts);
    } else {
        snprintf(buf, len, "/sdcard/note_%05u.txt", (unsigned)svc_time_now() % 100000u);
    }
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta == NULL) return;

    if (s_file[0] == '\0') {
        make_default_name(s_file, sizeof(s_file));
    }

    const char *text = lv_textarea_get_text(s_ta);
    if (text == NULL) return;

    if (svc_storage_write(s_file, text, strlen(text)) != ESP_OK) {
        fw_ui_toast("保存失败", 2000);
        return;
    }

    update_name();
    fw_ui_toast("已保存", 1500);
    ESP_LOGI(TAG, "saved %s (%u bytes)", s_file, (unsigned)strlen(text));
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta == NULL) return;

    lvgl_port_lock(0);
    lv_textarea_set_text(s_ta, "");
    lvgl_port_unlock();

    s_file[0] = '\0';
    update_name();
    fw_ui_toast("新文件（保存时自动命名）", 2000);
}

static void close_open_page(void)
{
    if (s_open_page == NULL) return;

    lvgl_port_lock(0);
    lv_obj_delete(s_open_page);
    s_open_page = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    free(s_paths);                          /* 列表项已随页面删除，路径缓冲可以放了 */
    s_paths = NULL;
}

static void open_row_cb(lv_event_t *e)
{
    const char *path = (const char *)lv_event_get_user_data(e);
    if (path == NULL) return;

    void *buf = NULL;
    size_t len = 0;
    if (svc_storage_read(path, &buf, &len) != ESP_OK) {
        fw_ui_toast("打开失败", 2000);
        return;
    }

    if (len >= ED_MAX_TEXT) {
        free(buf);
        fw_ui_toast("文件太大（上限 8 KB）", 2000);
        return;
    }

    lvgl_port_lock(0);
    lv_textarea_set_text(s_ta, (const char *)buf);
    lvgl_port_unlock();

    free(buf);

    strlcpy(s_file, path, sizeof(s_file));
    update_name();
    close_open_page();

    fw_ui_toast("已打开", 1500);
}

static void open_cb(lv_event_t *e)
{
    (void)e;

    if (s_open_page != NULL) {
        close_open_page();
        return;
    }

    if (s_paths == NULL) {
        s_paths = malloc((size_t)ED_LIST_MAX * ED_FILE_MAX);
    }
    if (s_paths == NULL) {
        fw_ui_toast("内存不足", 2000);
        return;
    }

    lvgl_port_lock(0);

    s_open_page = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_open_page, lv_pct(100), lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H);
    lv_obj_set_pos(s_open_page, 0, FW_STATUSBAR_H);
    lv_obj_set_scrollable(s_open_page, false);
    lv_obj_set_style_bg_color(s_open_page, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(s_open_page, 12, 0);
    lv_obj_set_style_pad_row(s_open_page, 8, 0);
    lv_obj_set_style_border_width(s_open_page, 0, 0);
    lv_obj_set_flex_flow(s_open_page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_open_page);
    lv_label_set_text(title, "打开文本文件");
    lv_obj_set_style_text_font(title, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);

    s_list = fw_ui_list(s_open_page, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    lvgl_port_unlock();

    /* 扫 .txt：TF 卡优先，再内置存储 */
    static const char *bases[] = { "/sdcard", "/internal" };
    int shown = 0;

    for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]) && shown < ED_LIST_MAX; b++) {
        svc_storage_iter_t it = NULL;
        if (svc_storage_iter_start(bases[b], &it) != ESP_OK) continue;

        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL && shown < ED_LIST_MAX) {
            if (entry->is_dir) continue;

            const char *dot = strrchr(entry->name, '.');
            if (dot == NULL || strcasecmp(dot, ".txt") != 0) continue;

            snprintf(s_paths[shown], ED_FILE_MAX, "%s/%.150s", bases[b], entry->name);
            lvgl_port_lock(0);
            fw_ui_list_add(s_list, entry->name, open_row_cb, s_paths[shown]);
            lvgl_port_unlock();
            shown++;
        }
        svc_storage_iter_end(it);
    }

    if (shown == 0) {
        lvgl_port_lock(0);
        fw_ui_list_add(s_list, "没有 .txt 文件", NULL, NULL);
        lvgl_port_unlock();
    }
}

/* ------------------------------ 生命周期 ------------------------------ */

static lv_obj_t *small_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 56, 36);
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

static void *editor_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    small_btn(bar, LV_SYMBOL_DIRECTORY, open_cb);
    small_btn(bar, LV_SYMBOL_SAVE, save_cb);
    small_btn(bar, LV_SYMBOL_PLUS, new_cb);

    s_name = lv_label_create(bar);
    lv_obj_set_style_text_font(s_name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_name, fw_theme_color_text_secondary(), 0);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_name, 100);

    s_ta = lv_textarea_create(body);
    lv_obj_set_width(s_ta, lv_pct(100));
    lv_obj_set_flex_grow(s_ta, 1);
    lv_textarea_set_max_length(s_ta, ED_MAX_TEXT);
    lv_textarea_set_one_line(s_ta, false);
    lv_textarea_set_placeholder_text(s_ta, "点这里输入…");
    lv_obj_set_style_text_font(s_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_ta, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_bg_color(s_ta, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(s_ta, 1, 0);
    lv_obj_set_style_border_color(s_ta, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(s_ta, 8, 0);
    lv_obj_add_event_cb(s_ta, ta_event_cb, LV_EVENT_FOCUSED, NULL);

    /* 软键盘：贴在屏幕底部、默认隐藏；盖上内容区不影响编辑（光标可能被挡住，接受） */
    s_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_kb, lv_pct(100), 150);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_secondary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_card(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, fw_theme_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_kb, fw_theme_color_border(), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_set_hidden(s_kb, true);

    lvgl_port_unlock();

    s_file[0] = '\0';
    update_name();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void editor_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_open_page != NULL) {
        lv_obj_delete(s_open_page);
        s_open_page = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_ta = NULL;
    s_kb = NULL;
    s_name = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    free(s_paths);
    s_paths = NULL;
}

/* 返回键：先收键盘，再关"打开"页，都没有才退出 */
static bool editor_on_back(void *ctx)
{
    (void)ctx;

    if (s_kb != NULL && !lv_obj_is_hidden(s_kb)) {
        keyboard_toggle(false);
        return true;
    }
    if (s_open_page != NULL) {
        close_open_page();
        return true;
    }
    return false;
}

const fw_app_desc_t app_editor_desc = {
    .name = "Editor",
    .title = "文本编辑",
    .icon_64 = &icon_home_editor,
    .symbol = LV_SYMBOL_EDIT,
    .on_create = editor_on_create,
    .on_destroy = editor_on_destroy,
    .on_back = editor_on_back,
};
