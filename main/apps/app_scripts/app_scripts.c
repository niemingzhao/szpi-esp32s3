/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Scripts（APP-SCRIPTS 脚本）
 *
 * 列出 TF 卡脚本目录里的脚本（`fw_script_scan`），点选运行；顶部显示当前运行状态并
 * 可停止。运行状态经 SVC_EVENT_SCRIPT_STARTED / SVC_EVENT_SCRIPT_STOPPED 通知。
 * 长按列表项可删除脚本（二次确认）；「下载新脚本」输入链接后经 svc_http_download
 * 存到 /sdcard/scripts/。
 *
 * 脚本列表放在堆上（几十条 × 每条几百字节），不在 App 里放大静态数组（AGENTS 4.21）。
 */

#include "app_scripts.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.scripts";

#define SCRIPT_MAX      32
#define DL_KB_HEIGHT    130
#define DL_NAME_MAX     128

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_state_row = NULL;
static lv_obj_t *s_list = NULL;

static fw_script_info_t *s_scripts = NULL;
static size_t s_count = 0;

/* 删除确认用的目标路径（对话框回调里使用） */
static char s_del_path[FW_SCRIPT_PATH_MAX] = { 0 };

/* 下载新脚本的链接输入浮层 */
static lv_obj_t *s_dl = NULL;
static lv_obj_t *s_dl_ta = NULL;
static bool s_downloading = false;

/* 前置声明（fill_list / scan / 删除确认互相引用） */
static void scan(void);
static void item_long_cb(lv_event_t *e);
static void del_confirm_cb(fw_dialog_btn_t btn, void *user);

static void show_state(void)
{
    if (s_state_row == NULL) return;

    const char *cur = fw_script_current();
    fw_ui_row_btn_value(s_state_row, (cur != NULL) ? cur : "未运行");
}

static void run_cb(lv_event_t *e)
{
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_count) return;

    if (fw_script_run(s_scripts[idx - 1].path) != ESP_OK) {
        fw_ui_toast("启动失败", 2000);
    }
}

/* 长按列表项：确认后删除（运行中的脚本不让删，避免把正在执行的 Lua 文件拔掉） */
static void item_long_cb(lv_event_t *e)
{
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_count || s_scripts == NULL) return;

    if (fw_script_is_running()) {
        fw_ui_toast("请先停止脚本", 2000);
        return;
    }

    strlcpy(s_del_path, s_scripts[idx - 1].path, sizeof(s_del_path));
    fw_ui_dialog(NULL, "删除脚本", s_scripts[idx - 1].name,
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, del_confirm_cb, NULL);
}

static void del_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;

    if (btn != FW_DIALOG_BTN_OK || s_del_path[0] == '\0') {
        return;
    }

    const esp_err_t err = svc_storage_remove(s_del_path);
    s_del_path[0] = '\0';

    fw_ui_toast((err == ESP_OK) ? "已删除" : "删除失败", 2000);
    if (err == ESP_OK) {
        scan();
    }
}

/* 只删除列表条目，保留 fw_ui_list 的标题（标题是第 0 个子对象） */
static void list_clear_items(lv_obj_t *list)
{
    if (list == NULL) return;

    uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = n; i > 1; i--) {
        lv_obj_delete(lv_obj_get_child(list, (int32_t)(i - 1)));
    }
}

static void fill_list(void)
{
    if (s_list == NULL) return;

    list_clear_items(s_list);

    if (s_scripts == NULL) {
        fw_ui_list_add(s_list, "内存不足", NULL, NULL);
        return;
    }
    if (s_count == 0) {
        fw_ui_list_add(s_list, "没有脚本（放到 TF 卡 scripts 目录）", NULL, NULL);
        return;
    }

    for (size_t i = 0; i < s_count; i++) {
        /* user_data 传下标 + 1（0 表示无效） */
        lv_obj_t *row = fw_ui_list_add(s_list, s_scripts[i].name, run_cb,
                                       (void *)(uintptr_t)(i + 1));
        if (row != NULL) {
            lv_obj_add_event_cb(row, item_long_cb, LV_EVENT_LONG_PRESSED,
                                (void *)(uintptr_t)(i + 1));
        }
    }
}

static void scan(void)
{
    size_t n = 0;

    if (s_scripts == NULL) return;

    if (fw_script_scan(s_scripts, SCRIPT_MAX, &n) != ESP_OK) {
        s_count = 0;
        fw_ui_toast("读取脚本目录失败", 2000);
    } else {
        s_count = n;
    }
    fill_list();
}

/* -------------------------------- 下载新脚本 -------------------------------- */

static void dl_close(void)
{
    if (s_dl != NULL) {
        lv_obj_delete(s_dl);
        s_dl = NULL;
        s_dl_ta = NULL;
    }
}

static void url_basename(const char *url, char *out, size_t len)
{
    const char *q = strchr(url, '?');
    const size_t n = q ? (size_t)(q - url) : strlen(url);

    const char *slash = NULL;
    for (size_t i = 0; i < n; i++) {
        if (url[i] == '/') slash = url + i;
    }

    const char *name = slash ? slash + 1 : url;
    if (slash == NULL || (size_t)(name - url) >= n || *name == '\0') {
        snprintf(out, len, "script.lua");
        return;
    }

    size_t m = n - (size_t)(name - url);
    if (m >= len) m = len - 1;
    memcpy(out, name, m);
    out[m] = '\0';
}

static bool is_lua(const char *name)
{
    const size_t n = strlen(name);
    if (n < 4) return false;

    const char *p = name + n - 4;
    return p[0] == '.' &&
           (p[1] == 'l' || p[1] == 'L') &&
           (p[2] == 'u' || p[2] == 'U') &&
           (p[3] == 'a' || p[3] == 'A');
}

/* 脚本管理器只列 .lua，链接没带扩展名时补上，否则下载完看不到 */
static void ensure_lua_ext(char *name, size_t len)
{
    if (is_lua(name)) return;

    size_t n = strlen(name);
    if (n + 4 >= len) {                 /* 留出 ".lua" 的位置 */
        n = len - 5;
        name[n] = '\0';
    }
    strcat(name, ".lua");
}

/* 下载结束（在 svc.http 任务里回调） */
static void dl_done_cb(size_t received, size_t total, esp_err_t err, bool done, void *user)
{
    (void)received;
    (void)total;
    (void)user;

    if (!done) return;                  /* 脚本文件小，不做下载进度 UI */

    s_downloading = false;

    /* 与 on_destroy 串行：持锁后再读 s_root / s_scripts，避免 App 退出时用到悬空数据 */
    lvgl_port_lock(0);
    if (s_root != NULL) {
        fw_ui_toast((err == ESP_OK) ? "下载完成" : "下载失败", 2500);
        if (err == ESP_OK) {
            scan();                     /* 脚本目录很小，持锁期间做一次扫描可以接受 */
        }
    }
    lvgl_port_unlock();
}

static void dl_start(void)
{
    if (s_dl_ta == NULL) return;
    if (s_downloading) {
        fw_ui_toast("正在下载…", 1500);
        return;
    }

    char url[192] = { 0 };
    snprintf(url, sizeof(url), "%s", lv_textarea_get_text(s_dl_ta));
    if (url[0] == '\0') {
        fw_ui_toast("请先输入链接", 2000);
        return;
    }

    char name[DL_NAME_MAX] = { 0 };
    url_basename(url, name, sizeof(name));
    ensure_lua_ext(name, sizeof(name));

    svc_storage_mkdir(FW_SCRIPT_DIR);   /* 已存在会失败，忽略 */

    char path[FW_SCRIPT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", FW_SCRIPT_DIR, name);

    s_downloading = true;
    if (svc_http_download(url, path, dl_done_cb, NULL) != ESP_OK) {
        s_downloading = false;
        fw_ui_toast("启动下载失败", 2000);
    } else {
        fw_ui_toast("正在下载…", 2000);
    }
}

static void dl_kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        dl_start();
        dl_close();
    } else if (code == LV_EVENT_CANCEL) {
        dl_close();
    }
}

static void dl_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_dl != NULL) return;

    s_dl = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_dl);
    lv_obj_set_size(s_dl, lv_pct(100), lv_pct(100));
    lv_obj_center(s_dl);
    lv_obj_set_style_bg_color(s_dl, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_dl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_dl, 8, 0);
    lv_obj_set_scrollable(s_dl, false);

    lv_obj_t *lb = lv_label_create(s_dl);
    lv_label_set_text(lb, "脚本链接");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 0, 0);

    s_dl_ta = lv_textarea_create(s_dl);
    lv_textarea_set_one_line(s_dl_ta, true);
    lv_textarea_set_placeholder_text(s_dl_ta, "http://…/demo.lua");
    lv_obj_set_size(s_dl_ta, lv_pct(100), 36);
    lv_obj_align(s_dl_ta, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_font(s_dl_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_dl_ta, fw_theme_color_text_primary(), 0);

    lv_obj_t *kb = lv_keyboard_create(s_dl);
    lv_obj_set_size(kb, lv_pct(100), DL_KB_HEIGHT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_dl_ta);
    lv_obj_add_event_cb(kb, dl_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, dl_kb_cb, LV_EVENT_CANCEL, NULL);
}

/* ---------------------------------- 事件 ---------------------------------- */

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    scan();
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    fw_script_stop();
}

static void on_script_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    show_state();
    lvgl_port_unlock();
}

static void *scripts_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_state_row = fw_ui_row_btn(body, LV_SYMBOL_PLAY, "当前脚本", NULL, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_CLOSE, "停止", stop_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_REFRESH, "刷新列表", refresh_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_DOWNLOAD, "下载新脚本", dl_open_cb, NULL);

    s_list = fw_ui_list(body, "脚本（长按删除）");

    if (s_scripts == NULL) {
        s_scripts = malloc(sizeof(fw_script_info_t) * SCRIPT_MAX);
    }

    show_state();
    scan();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void scripts_on_start(void *ctx)
{
    (void)ctx;
    svc_event_bus_subscribe(SVC_EVENT_SCRIPT_STARTED, on_script_evt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_SCRIPT_STOPPED, on_script_evt, NULL);

    lvgl_port_lock(0);
    show_state();
    lvgl_port_unlock();
}

static void scripts_on_pause(void *ctx)
{
    (void)ctx;
    svc_event_bus_unsubscribe(SVC_EVENT_SCRIPT_STARTED, on_script_evt);
    svc_event_bus_unsubscribe(SVC_EVENT_SCRIPT_STOPPED, on_script_evt);
}

static void scripts_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_dl = NULL;                    /* 随根屏一起删掉，避免悬空 */
    s_dl_ta = NULL;
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_state_row = NULL;
    s_list = NULL;
    free(s_scripts);                /* 放锁内，和下载完成回调串行 */
    s_scripts = NULL;
    s_count = 0;
    lvgl_port_unlock();

    s_downloading = false;
    s_del_path[0] = '\0';
}

const fw_app_desc_t app_scripts_desc = {
    .name = "Scripts",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_EDIT,
    .on_create = scripts_on_create,
    .on_start = scripts_on_start,
    .on_pause = scripts_on_pause,
    .on_destroy = scripts_on_destroy,
};
