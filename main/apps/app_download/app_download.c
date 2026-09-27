/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Download（下载器）
 *
 * 输入 http(s) 链接，流式下载到 TF 卡根目录（文件名取 URL 最后一段；.lua 存到脚本目录）。
 * 页面两块：两行操作 + 一块进度面板（面板标题就是状态文字，进度条是百分比）。
 * 下载中第一行变「暂停」、第二行变「停止」；暂停后第一行是「继续」（带 Range 续传），
 * 停掉会删掉半截文件。链接用浮层改：提示行 + 输入框 + 取消，键盘挂在根屏上、点输入框
 * 才弹出。
 *
 * 进度回调跑在 svc.http 任务里（栈 6144），所以回调只把数字记到静态变量，再经
 * lvgl_port_lock(超时) + lv_async_call() 交给 LVGL 任务去碰界面 —— 不在回调里建控件、
 * 排版。中途的刷新拿不到锁就丢一次；结束那一次用阻塞锁保证送到。
 */

#include "app_download.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.download";

#define DL_URL_MAX     192      /* 链接缓冲 */
#define DL_PATH_MAX    224      /* 落盘路径缓冲（服务侧上限 256） */
#define DL_NAME_MAX    128
#define DL_KB_H        120      /* 软键盘高度，与 Wi-Fi / 天气浮层一致 */
#define DL_OVL_PAD     8        /* 浮层内边距；键盘与浮层同宽，也按它留边 */
#define DL_OVL_TOP     30       /* 让开状态栏 */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_row_url = NULL;      /* 第一行：链接 / 暂停 / 继续 */
static lv_obj_t *s_row_dl = NULL;       /* 第二行：开始下载 / 停止 */
static lv_obj_t *s_row_url_icon = NULL;
static lv_obj_t *s_row_url_label = NULL;
static lv_obj_t *s_row_dl_icon = NULL;
static lv_obj_t *s_row_dl_label = NULL;
static lv_obj_t *s_bar = NULL;          /* 进度面板（fw_ui_progress_bar 返回的卡片） */
static lv_obj_t *s_overlay = NULL;      /* 链接输入浮层 */
static lv_obj_t *s_ta = NULL;           /* 浮层里的输入框 */
static lv_obj_t *s_kb = NULL;           /* 软键盘（挂根屏，默认收起） */

static char s_url[DL_URL_MAX] = "";
static char s_saved[DL_PATH_MAX] = "";
static bool s_cancelling = false;       /* 这一轮是被"停止"结束的，收尾时别报失败 */

/* 下面这几个由 svc.http 任务写、LVGL 任务读，用 volatile 即可（只是显示用的数字）。
 * 下载在跑与否以 svc_http_download_state() 为准，不另设标志 */
static volatile bool s_done = false;
static volatile esp_err_t s_result = ESP_OK;
static volatile bool s_size_known = true;   /* total 未知（分块传输）时按 KB 显示 */
static volatile uint8_t s_pct = 0;
static volatile size_t s_kb_done = 0;
static bool s_ui_queued = false;            /* 已经排了一次界面刷新，别堆 */
static uint32_t s_gen = 0;                  /* 界面重建代数：过期的异步回调直接丢 */

/* 前置声明 */
static void start_download(void);
static void update_buttons(void);

/* ------------------------------- 小工具 ------------------------------- */

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
        snprintf(out, len, "download.bin");
        return;
    }

    size_t m = n - (size_t)(name - url);
    if (m >= len) m = len - 1;
    memcpy(out, name, m);
    out[m] = '\0';
}

/* 脚本（.lua，不分大小写）放到脚本目录，其余文件放 TF 卡根目录 */
static void save_path_of(const char *name, char *out, size_t len)
{
    const size_t n = strlen(name);

    if (n >= 4 && name[n - 4] == '.' &&
        (name[n - 3] == 'l' || name[n - 3] == 'L') &&
        (name[n - 2] == 'u' || name[n - 2] == 'U') &&
        (name[n - 1] == 'a' || name[n - 1] == 'A')) {
        svc_storage_mkdir(FW_SCRIPT_DIR);       /* 已存在会失败，忽略 */
        snprintf(out, len, "%s/%s", FW_SCRIPT_DIR, name);
    } else {
        snprintf(out, len, "/sdcard/%s", name);
    }
}

/* 改一行的图标 / 文字（列的数据用 fw_ui_row_btn_value() 改） */
static void row_set(lv_obj_t *icon, lv_obj_t *label, const lv_image_dsc_t *src, const char *text)
{
    if (icon != NULL) lv_image_set_src(icon, src);
    if (label != NULL) lv_label_set_text(label, text);
}

/* 两行按钮按下载状态换文案：空闲=链接/开始下载；下载中=暂停/停止；暂停中=继续/停止 */
static void update_buttons(void)
{
    if (s_row_url_label == NULL || s_row_dl_label == NULL) return;

    switch (svc_http_download_state()) {
    case SVC_HTTP_DL_RUNNING:
        row_set(s_row_url_icon, s_row_url_label, &icon_ui_pause, "暂停");
        row_set(s_row_dl_icon, s_row_dl_label, &icon_ui_close, "停止");
        break;
    case SVC_HTTP_DL_PAUSED:
        row_set(s_row_url_icon, s_row_url_label, &icon_ui_play, "继续");
        row_set(s_row_dl_icon, s_row_dl_label, &icon_ui_close, "停止");
        break;
    default:
        row_set(s_row_url_icon, s_row_url_label, &icon_ui_link, "链接");
        row_set(s_row_dl_icon, s_row_dl_label, &icon_ui_download, "开始下载");
        break;
    }
}

/* ------------------------------- 进度 ------------------------------- */

/* 在 LVGL 任务里落到界面上；user 是排这次刷新时的界面代数 */
static void dl_ui_apply(void *user)
{
    s_ui_queued = false;

    /* busy 与界面代数无关：换主题重建期间下完的，回来也要能看到"没在下载" */
    if (s_done) s_done = false;

    if ((uint32_t)(uintptr_t)user != s_gen) return;     /* 界面已经重建过，丢掉 */
    if (s_bar == NULL) return;

    update_buttons();

    const svc_http_dl_state_t st = svc_http_download_state();
    char txt[40];

    if (s_cancelling) {
        fw_ui_progress_set(s_bar, 0);
        fw_ui_progress_title(s_bar, "已停止");
        fw_ui_toast("已停止", 1500);
        s_cancelling = false;
        return;
    }

    if (st == SVC_HTTP_DL_PAUSED) {
        snprintf(txt, sizeof(txt), "已暂停 %u%%", (unsigned)s_pct);
        fw_ui_progress_title(s_bar, txt);
        return;
    }

    if (st == SVC_HTTP_DL_IDLE) {
        /* 真的结束了 */
        if (s_result == ESP_OK) {
            fw_ui_progress_set(s_bar, 100);
            fw_ui_progress_title(s_bar, s_saved);
            fw_ui_toast("下载完成", 2500);
        } else {
            /* 带上 HTTP 状态码：403 / 401 是服务器拒绝（要登录态 / 链接过期），
             * 和"设备连不上"要能分得开 */
            const int http = svc_http_download_status();
            char txt[40];
            if (http == 401 || http == 403) {
                snprintf(txt, sizeof(txt), "服务器拒绝（HTTP %d）", http);
            } else if (http != 0 && (http < 200 || http >= 300)) {
                snprintf(txt, sizeof(txt), "请求失败（HTTP %d）", http);
            } else {
                snprintf(txt, sizeof(txt), "下载失败");
            }
            fw_ui_progress_set(s_bar, 0);
            fw_ui_progress_title(s_bar, txt);
            fw_ui_toast("下载失败", 2500);
        }
        return;
    }

    /* 传输中 */
    if (s_size_known) {
        snprintf(txt, sizeof(txt), "已下载 %u%%", (unsigned)s_pct);
        fw_ui_progress_set(s_bar, s_pct);
    } else {
        snprintf(txt, sizeof(txt), "已下载 %u KB", (unsigned)s_kb_done);
    }
    fw_ui_progress_title(s_bar, txt);
}

/* 进度 / 结束回调（在 svc.http 任务里执行，不是 LVGL 任务） */
static void dl_progress(size_t received, size_t total, esp_err_t err, bool done, void *user)
{
    (void)user;

    if (!done) {
        if (total == 0) {
            s_size_known = false;
            s_kb_done = received / 1024;
        } else {
            size_t pct = (size_t)(((uint64_t)received * 100u) / total);
            s_size_known = true;
            s_pct = (pct > 100) ? 100 : (uint8_t)pct;
        }

        /* 传输中：已经在排队就等它跑完，拿不到锁就丢这次 */
        if (s_ui_queued) return;
        if (!lvgl_port_lock(100)) return;
    } else {
        s_done = true;
        s_result = err;
        /* 结束必须送到，这里等锁（回调任务不是 LVGL 任务，阻塞没问题） */
        lvgl_port_lock(0);
    }

    s_ui_queued = true;
    lv_async_call(dl_ui_apply, (void *)(uintptr_t)s_gen);
    lvgl_port_unlock();
}

/* 服务从响应里推出的文件名（可能为空）→ 决定最终落盘路径：
 * 有名字就用名字（.lua 仍按规则落到脚本目录），没有才退回 URL 最后一段 */
static esp_err_t name_cb(const char *name, char *out_path, size_t path_len, void *user)
{
    (void)user;

    char file[DL_NAME_MAX] = { 0 };
    if (name != NULL && name[0] != '\0') {
        strlcpy(file, name, sizeof(file));
    } else {
        url_basename(s_url, file, sizeof(file));
    }

    save_path_of(file, out_path, path_len);
    strlcpy(s_saved, out_path, sizeof(s_saved));    /* 完成后界面显示的是真实落盘路径 */
    return ESP_OK;
}

static void start_download(void)
{
    if (s_url[0] == '\0') {
        fw_ui_toast("请先输入链接", 2000);
        return;
    }

    char name[DL_NAME_MAX] = { 0 };
    url_basename(s_url, name, sizeof(name));
    save_path_of(name, s_saved, sizeof(s_saved));

    s_cancelling = false;
    s_done = false;
    s_result = ESP_OK;
    s_size_known = true;
    s_pct = 0;
    s_kb_done = 0;
    s_ui_queued = false;

    fw_ui_progress_set(s_bar, 0);
    fw_ui_progress_title(s_bar, "启动中…");

    /* 回调可能很快就到，先置好状态再启动；真正的文件名由 name_cb 按响应决定 */
    if (svc_http_download(s_url, s_saved, dl_progress, name_cb, NULL) != ESP_OK) {
        fw_ui_progress_title(s_bar, "启动失败");
        fw_ui_toast("启动下载失败", 2000);
    }
    update_buttons();
}

/* ------------------------------- 链接浮层 ------------------------------- */

static void kb_hide(void)
{
    if (s_kb != NULL) {
        lv_obj_set_hidden(s_kb, true);
        lv_keyboard_set_textarea(s_kb, NULL);   /* 解绑，免得下次打开时输入框已带焦点 */
    }
}

static void overlay_close(void)
{
    kb_hide();
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_ta = NULL;
    }
}

/* 点浮层空白处：收键盘、输入框失焦，浮层留着方便接着改 */
static void overlay_click_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_overlay) return;

    kb_hide();
    if (s_ta != NULL) lv_obj_remove_state(s_ta, LV_STATE_FOCUSED);
}

/* 点输入框弹键盘。用 CLICKED 而不是 FOCUSED：收起来再点，输入框可能还是焦点态，
 * FOCUSED 不会再发，键盘就弹不出来 */
static void ta_tap_cb(lv_event_t *e)
{
    (void)e;
    if (s_kb == NULL) return;

    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_set_hidden(s_kb, false);
    lv_obj_move_foreground(s_kb);
}

static void ovl_cancel_cb(lv_event_t *e)
{
    (void)e;
    overlay_close();
}

static void kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        /* 回车 = 确认这次输入（带键盘的浮层不放"确定"按钮） */
        if (s_ta != NULL) {
            snprintf(s_url, sizeof(s_url), "%s", lv_textarea_get_text(s_ta));
            fw_ui_row_btn_value(s_row_url, s_url);
        }
        overlay_close();
    } else if (code == LV_EVENT_CANCEL) {
        kb_hide();
    }
}

static void overlay_open(void)
{
    if (s_overlay != NULL) return;

    s_overlay = lv_obj_create(s_root);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, DL_OVL_PAD, 0);
    lv_obj_set_style_pad_top(s_overlay, DL_OVL_TOP, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    /* 第一行：提示（撑满）+ 取消 */
    lv_obj_t *hint_row = lv_obj_create(s_overlay);
    lv_obj_set_size(hint_row, lv_pct(100), 30);
    lv_obj_set_scrollable(hint_row, false);
    lv_obj_set_style_bg_opa(hint_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hint_row, 0, 0);
    lv_obj_set_style_pad_all(hint_row, 0, 0);
    lv_obj_set_style_pad_column(hint_row, 8, 0);
    lv_obj_set_flex_flow(hint_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hint_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hint = lv_label_create(hint_row);
    lv_obj_set_flex_grow(hint, 1);
    lv_label_set_text(hint, "下载链接");
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);

    fw_ui_btn(hint_row, "取消", 50, false, ovl_cancel_cb, NULL);

    /* 第二行：输入框 */
    s_ta = fw_ui_textarea(s_overlay, "http://… （.lua 会存到脚本目录）");
    lv_obj_set_width(s_ta, lv_pct(100));
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 36);
    lv_textarea_set_text(s_ta, s_url);
    lv_obj_add_event_cb(s_ta, ta_tap_cb, LV_EVENT_CLICKED, NULL);

    /* 键盘挂在根屏上：浮层是给内容排版的，键盘进去会被算进布局 */
    if (s_kb == NULL) {
        s_kb = lv_keyboard_create(s_root);
        if (s_kb != NULL) {
            lv_obj_set_size(s_kb, lv_display_get_horizontal_resolution(lv_display_get_default())
                                  - 2 * DL_OVL_PAD, DL_KB_H);
            lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -DL_OVL_PAD);
            lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
            lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
        }
    }
    if (s_kb != NULL) lv_obj_set_hidden(s_kb, true);
}

/* ---------------------------------- 事件 ---------------------------------- */

/* 第一行：空闲改链接；下载中暂停；暂停中继续 */
static void url_cb(lv_event_t *e)
{
    (void)e;

    switch (svc_http_download_state()) {
    case SVC_HTTP_DL_RUNNING:
        svc_http_download_pause();
        break;
    case SVC_HTTP_DL_PAUSED:
        svc_http_download_resume();
        break;
    default:
        overlay_open();
        return;
    }
    update_buttons();
}

/* 第二行：空闲开始下载；下载中 / 暂停中停止 */
static void dl_cb(lv_event_t *e)
{
    (void)e;

    const svc_http_dl_state_t st = svc_http_download_state();

    if (st == SVC_HTTP_DL_IDLE) {
        start_download();
        return;
    }

    if (st == SVC_HTTP_DL_PAUSED) {
        /* 已经停住了：服务马上删掉半截文件，界面这里直接收尾（不会再有回调） */
        svc_http_download_cancel();
        s_cancelling = false;
        if (s_bar != NULL) {
            fw_ui_progress_set(s_bar, 0);
            fw_ui_progress_title(s_bar, "已停止");
        }
        fw_ui_toast("已停止", 1500);
    } else {
        /* 传输中：让任务收尾，下一次回调里更新界面 */
        s_cancelling = true;
        svc_http_download_cancel();
    }

    update_buttons();
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *download_on_create(void)
{
    lvgl_port_lock(0);

    s_gen++;
    s_done = false;
    s_result = ESP_OK;
    s_size_known = true;
    s_pct = 0;
    s_kb_done = 0;
    s_ui_queued = false;
    s_cancelling = false;
    s_overlay = NULL;
    s_ta = NULL;
    s_kb = NULL;

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);
    lv_obj_set_scrollable(body, false);     /* 三块固定高度，别让它能滚 */

    lv_obj_t *grp = fw_ui_group(body);
    s_row_url = fw_ui_row_btn_img(grp, &icon_ui_link, "链接", url_cb, NULL);
    s_row_dl = fw_ui_row_btn_img(grp, &icon_ui_download, "开始下载", dl_cb, NULL);
    fw_ui_group_end(grp);

    /* 行内对象顺序固定：0 = 图标，1 = 文字，2 = 右侧数值 */
    s_row_url_icon = lv_obj_get_child(s_row_url, 0);
    s_row_url_label = lv_obj_get_child(s_row_url, 1);
    s_row_dl_icon = lv_obj_get_child(s_row_dl, 0);
    s_row_dl_label = lv_obj_get_child(s_row_dl, 1);

    fw_ui_row_btn_value(s_row_url, s_url[0] ? s_url : "(未输入)");

    /* 进度面板：标题就是状态，空闲时也在，避免下载中界面突然长高 */
    s_bar = fw_ui_progress_bar(body, "就绪");
    fw_ui_progress_set(s_bar, 0);

    /* 换主题重建时如果下载还在跑，按钮要接着显示"暂停 / 停止" */
    update_buttons();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (dl state=%d)", (int)svc_http_download_state());
    return s_root;
}

static void download_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    /* 浮层和键盘都挂在根屏上，随根屏一起删掉；这里先把句柄清空，免得回调碰到悬空指针 */
    s_overlay = NULL;
    s_ta = NULL;
    s_kb = NULL;
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_row_url = NULL;
    s_row_dl = NULL;
    s_row_url_icon = NULL;
    s_row_url_label = NULL;
    s_row_dl_icon = NULL;
    s_row_dl_label = NULL;
    s_bar = NULL;
    lvgl_port_unlock();

    /* 下载还在跑的话让它继续跑完（回调只写静态变量，界面句柄已经置空） */
    s_ui_queued = false;
}

/* 返回键：浮层开着就先关浮层（返回 true 表示 App 内已处理），别直接退回桌面 */
static bool download_on_back(void *ctx)
{
    (void)ctx;

    if (s_overlay != NULL) {
        overlay_close();
        return true;
    }
    return false;
}

const fw_app_desc_t app_download_desc = {
    .name = "Download",
    .title = "下载器",
    .icon = &icon_home_download,
    .on_create = download_on_create,
    .on_destroy = download_on_destroy,
    .on_back = download_on_back,
};
