/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Editor（文本编辑）
 *
 * 纯文本编辑：整块读文件（上限 ED_MAX_TEXT），软键盘输入（LVGL 自带键盘，只有拉丁字符，
 * 中文没有输入法，中文内容靠打开已有文件显示），保存回原文件。
 *   顶部：当前文件名（有未保存改动时带 *）+ 打开 / 保存 / 新建
 *   打开页：列出两个存储里的文本类文件（含子目录，脚本也按文本打开），有多少列多少
 *   键盘：弹出时收起工具栏、编辑区压到键盘上方（保留约三行可见），键盘四周留 8
 * 没有文件名时保存自动命名 note_<时间>.txt；离开前台不销毁，换主题重建时把未保存的
 * 内容先存回文件、重建后再读回来，不丢改动；换文件前也会先把改动存回原文件。
 * 支持从文件管理带 Editor?path=<绝对路径> 启动直接打开该文件。
 *
 * 候选路径放堆上、按目录内容成倍扩（不在 App 里放大静态数组）。
 */

#include "app_editor.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "app.editor";

#define ED_MAX_TEXT    16384    /* 单文件字符上限（textarea 上限与整块读上限） */
#define ED_FILE_MAX    200      /* 路径缓冲，与文件管理的 PATH_MAX_LEN 一致 */
#define ED_LIST_INIT   32       /* "打开"页起始容量（按需成倍扩） */
#define ED_LIST_MAX    512      /* 上限 */
#define ED_SCAN_DEPTH  3        /* 目录递归层数 */
#define ED_KB_H        120      /* 软键盘高度 */
#define ED_KB_MARGIN   8        /* 键盘四周留白（与文件管理 / Wi-Fi 输入浮层一致） */

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_bar = NULL;          /* 顶部工具栏（弹键盘时藏起来） */
static lv_obj_t *s_ta = NULL;
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_name = NULL;
static lv_obj_t *s_open_page = NULL;
static lv_obj_t *s_list = NULL;

static char s_file[ED_FILE_MAX] = "";   /* 当前文件的完整路径，空 = 未命名 */
static bool s_dirty = false;            /* 有未保存改动 */
static bool s_rebuilding = false;       /* on_destroy 到下一次 on_create：换主题原地重建 */

/* 候选路径放堆上（按目录内容成倍扩）；列表项的 user_data 存下标，
 * 因为扩容用的是 realloc，指向数组元素的指针会失效 */
static char (*s_paths)[ED_FILE_MAX] = NULL;
static size_t s_paths_cap = 0;
static size_t s_open_count = 0;             /* "打开"页当前列出的项数 */
static fw_ui_stage_t *s_stage = NULL;       /* "打开"页列表分段构建 */

/* 前置声明 */
static void update_name(void);
static void close_open_page(void);
static bool load_file(const char *path, bool notify);
static void keyboard_show(bool show);
static void open_cb(lv_event_t *e);

/* ------------------------------- 小工具 ------------------------------- */

/* 保证候选路径数组至少能放 need 项；不够就成倍扩（上限 ED_LIST_MAX） */
static bool list_reserve(size_t need)
{
    if (need <= s_paths_cap) return true;
    if (need > ED_LIST_MAX) return false;

    size_t cap = (s_paths_cap > 0) ? s_paths_cap : ED_LIST_INIT;
    while (cap < need) cap *= 2;
    if (cap > ED_LIST_MAX) cap = ED_LIST_MAX;

    void *p = realloc(s_paths, cap * ED_FILE_MAX);
    if (p == NULL) return false;

    s_paths = p;
    s_paths_cap = cap;
    return true;
}

/* 文本类扩展名：和 app_file 的文本类型保持一致（脚本也按文本打开） */
static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return (dot != NULL) && (strcasecmp(dot, ext) == 0);
}

static bool is_text_name(const char *name)
{
    static const char *const EXTS[] = {
        ".txt", ".md", ".markdown", ".log", ".csv", ".json", ".ini", ".cfg", ".conf",
        ".toml", ".xml", ".yml", ".yaml", ".html", ".css", ".js", ".py", ".lua", ".sh",
        ".bat", ".ps1", ".c", ".h", ".cpp", ".hpp", ".ino",
    };

    for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
        if (has_ext(name, EXTS[i])) return true;
    }
    return false;
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? slash + 1 : path;
}

/* 列表里显示的名字：去掉存储根前缀（子目录里的文件带一级目录，便于区分） */
static const char *rel_name(const char *path)
{
    if (strncmp(path, "/sdcard/", 8) == 0) return path + 8;
    if (strncmp(path, "/internal/", 10) == 0) return path + 10;
    return path;
}

static void update_name(void)
{
    if (s_name == NULL) return;

    const char *base = (s_file[0] != '\0') ? path_basename(s_file) : "未命名";
    char buf[ED_FILE_MAX + 4];
    snprintf(buf, sizeof(buf), "%s%s", base, s_dirty ? " *" : "");
    lv_label_set_text(s_name, buf);
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

/* ------------------------------- 键盘 ------------------------------- */

/* 键盘弹出时收起工具栏，把编辑区压到键盘上方（保留约三行可见） */
static void keyboard_show(bool show)
{
    if (s_kb == NULL || s_ta == NULL || s_bar == NULL) return;

    const int32_t body_h =
        lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H;

    lvgl_port_lock(0);

    if (show) {
        /* 内容区 − 上边距 − 键盘（含留白）− 一点间隙：光标始终在键盘上方 */
        lv_obj_set_hidden(s_bar, true);
        lv_obj_set_flex_grow(s_ta, 0);
        lv_obj_set_height(s_ta, body_h - 12 - (ED_KB_H + ED_KB_MARGIN) - 4);
        lv_obj_set_hidden(s_kb, false);
        lv_keyboard_set_textarea(s_kb, s_ta);       /* 重新接管焦点，光标可见 */
    } else {
        lv_obj_set_hidden(s_kb, true);
        lv_obj_set_height(s_ta, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(s_ta, 1);
        lv_obj_set_hidden(s_bar, false);
        /* 收起时解除绑定：一并清掉输入框的 FOCUSED，否则再点输入框不再发
         * LV_EVENT_FOCUSED，键盘就弹不出来了 */
        lv_keyboard_set_textarea(s_kb, NULL);
    }

    lvgl_port_unlock();
}

static void kb_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_READY:
    case LV_EVENT_CANCEL:
        keyboard_show(false);
        break;
    default:
        break;
    }
}

static void ta_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_FOCUSED:
    case LV_EVENT_CLICKED:      /* 键盘收起后 FOCUSED 不会再发，靠点击重新弹出 */
        keyboard_show(true);
        break;
    case LV_EVENT_VALUE_CHANGED:
        if (!s_dirty) {
            s_dirty = true;
            update_name();
        }
        break;
    default:
        break;
    }
}

/* ------------------------------- 文件 ------------------------------- */

/* 保存到当前文件；没有文件名时自动命名。失败返回 false（用于自动保存，
 * 调用方据此决定要不要继续往下走） */
static bool save_file(bool notify)
{
    if (s_ta == NULL) return false;

    if (s_file[0] == '\0') make_default_name(s_file, sizeof(s_file));

    const char *text = lv_textarea_get_text(s_ta);
    if (text == NULL) return false;

    if (svc_storage_write(s_file, text, strlen(text)) != ESP_OK) {
        if (notify) fw_ui_toast("保存失败", 2000);
        return false;
    }

    s_dirty = false;
    update_name();

    if (notify) fw_ui_toast("已保存", 1500);
    ESP_LOGI(TAG, "saved %s (%u bytes)", s_file, (unsigned)strlen(text));
    return true;
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    save_file(true);
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    if (s_ta == NULL) return;

    /* 新文件之前先把未保存的改动存回原文件；存不下就别清，免得更丢 */
    if (s_dirty && s_file[0] != '\0' && !save_file(false)) {
        fw_ui_toast("当前改动保存失败，未新建", 2000);
        return;
    }

    lvgl_port_lock(0);
    lv_textarea_set_text(s_ta, "");
    lvgl_port_unlock();

    s_file[0] = '\0';
    s_dirty = false;
    update_name();
    fw_ui_toast("新文件（保存时自动命名）", 2000);
}

/* 读一个文本文件进编辑区；成功后它就是"当前文件"（保存写回这里） */
static bool load_file(const char *path, bool notify)
{
    if (s_ta == NULL || path == NULL) return false;

    /* 先看大小再读：编辑区只有 ED_MAX_TEXT，过大的文件读进来也是白读一遍
     * （最多 1 MB），不如直接告诉用户 */
    size_t size = 0;
    if (svc_storage_exists(path, &size) == ESP_OK && size >= ED_MAX_TEXT) {
        if (notify) fw_ui_toast("文件太大（上限 16 KB）", 2000);
        ESP_LOGW(TAG, "open %s refused: %u bytes", path, (unsigned)size);
        return false;
    }

    void *buf = NULL;
    size_t len = 0;
    if (svc_storage_read(path, &buf, &len) != ESP_OK) {
        if (notify) fw_ui_toast("打开失败", 2000);
        return false;
    }

    if (len >= ED_MAX_TEXT) {
        free(buf);
        if (notify) fw_ui_toast("文件太大（上限 16 KB）", 2000);
        return false;
    }

    lvgl_port_lock(0);
    /* 设了 max_length 之后 lv_textarea_set_text() 会逐字符插入，每插一个字都要重排一次
     * 整个标签：几 KB 的文本要卡好几秒，界面在这期间不刷新，看上去就是"编辑框是空的"。
     * 临时清掉上限让它走整块设置，读完再恢复（上限只管输入）。 */
    lv_textarea_set_max_length(s_ta, 0);
    lv_textarea_set_text(s_ta, (const char *)buf);
    lv_textarea_set_cursor_pos(s_ta, 0);        /* 停在开头：长文件从第一行看起 */
    lv_textarea_set_max_length(s_ta, ED_MAX_TEXT);
    lvgl_port_unlock();

    free(buf);

    if (path != s_file) strlcpy(s_file, path, sizeof(s_file));   /* 重建时 path 就是 s_file */
    s_dirty = false;
    update_name();
    close_open_page();

    if (notify) fw_ui_toast("已打开", 1500);
    return true;
}

/* 来自文件管理（"Editor?path=/xxx.txt"）：直接打开这个文件。
 * 返回 true 表示这次是冲着一个文件来的（哪怕已经在显示它了）。 */
static bool open_external(void)
{
    const char *args = fw_app_mgr_get_args();
    const char *p = (args != NULL) ? strstr(args, "path=") : NULL;

    if (p == NULL) return false;

    p += 5;
    if (p[0] != '/') return false;

    if (!is_text_name(p)) {
        /* 文件管理按扩展名把它送到这里，说明这个扩展名不在支持列表里：以前是静默返回，
         * 界面看着像"打开了但是空的"，这里明说一句 */
        fw_ui_toast("这个类型不在文本编辑器里打开", 2500);
        return false;
    }

    /* 同一个文件且编辑区已经有内容：保持现状（包括未保存的改动）。
     * 编辑区是空的说明上次没加载成功（比如当时文件还是空的），这里重试一次 */
    if (strcmp(p, s_file) == 0 && s_ta != NULL && lv_textarea_get_text(s_ta)[0] != '\0') {
        return true;
    }

    /* 换文件前把当前未保存的改动存回去（存不下就别换，免得改动被覆盖掉） */
    if (s_dirty && s_file[0] != '\0' && !save_file(false)) {
        fw_ui_toast("当前改动保存失败，未打开", 2000);
        return false;
    }

    return load_file(p, true);
}

/* 首次创建 / 换主题重建之后，编辑区该显示什么：
 *   换主题重建 → 把当前文件读回来（on_destroy 已把未保存的改动存回文件）
 *   首次创建   → 读启动参数里的 path=（文件管理 / 脚本管理送过来的文件）
 * 进前台有两条路径（首次走 on_create，从返回栈回来只走 on_resume），两边都要处理参数。 */
static void open_initial(void)
{
    if (s_rebuilding) {
        s_rebuilding = false;
        if (s_file[0] != '\0') {
            load_file(s_file, false);
            return;
        }
        update_name();
        return;
    }

    if (open_external()) return;        /* 启动参数带来了文件 */

    /* 没有参数：把上次编辑的文件读回来，实在没有就留空 */
    if (s_file[0] != '\0') {
        load_file(s_file, false);
    } else {
        update_name();
    }
}

/* ------------------------------- 打开页 ------------------------------- */

static void close_open_page(void)
{
    if (s_open_page == NULL) return;

    /* 先停分段构建：下面的回调会用到已经被删的列表 */
    fw_ui_stage_stop(s_stage);

    lvgl_port_lock(0);
    lv_obj_delete(s_open_page);
    s_open_page = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    free(s_paths);                          /* 列表项已随页面删除，路径缓冲可以放了 */
    s_paths = NULL;
    s_paths_cap = 0;
    s_open_count = 0;
}

static void open_row_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_open_count) return;

    load_file(s_paths[idx], true);
}

#define ED_BUILD_CHUNK  12      /* 分段构建：每批建几项 */

/* 长按删除：先把目标拷出来（列表会在重建后失效），确认后再删并刷新列表 */
static char s_del_path[ED_FILE_MAX] = "";
static char s_del_name[ED_FILE_MAX] = "";

static void del_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    if (svc_storage_remove(s_del_path) != ESP_OK) {
        fw_ui_toast("删除失败", 2000);
        return;
    }

    /* 删掉的正好是当前编辑的文件：清掉文件名，下次保存会另存新文件 */
    if (strcmp(s_del_path, s_file) == 0) {
        s_file[0] = '\0';
        update_name();
    }

    fw_ui_toast("已删除", 1500);
    ESP_LOGI(TAG, "removed %s", s_del_path);

    close_open_page();
    open_cb(NULL);                          /* 重新列出剩下的文件 */
}

static void open_row_long_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_open_count) return;

    strlcpy(s_del_path, s_paths[idx], sizeof(s_del_path));
    strlcpy(s_del_name, rel_name(s_paths[idx]), sizeof(s_del_name));

    char msg[ED_FILE_MAX + 64];
    snprintf(msg, sizeof(msg), "删除「%s」？删除后无法恢复。", s_del_name);
    fw_ui_dialog(NULL, "删除文件", msg, FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 del_confirm_cb, NULL);
}

/* 分段建第 idx 项（由 fw_ui_stage_start 调，已在 LVGL 锁内） */
static void build_open_row(size_t idx, void *user)
{
    (void)user;
    if (s_list == NULL || idx >= s_open_count) return;

    lv_obj_t *item = fw_ui_list_add(s_list, rel_name(s_paths[idx]), open_row_cb,
                                    (void *)(uintptr_t)idx);
    if (item != NULL) {
        lv_obj_add_event_cb(item, open_row_long_cb, LV_EVENT_LONG_PRESSED,
                            (void *)(uintptr_t)idx);
    }
}

/* 全部建完：没有文本文件时给一行提示 */
static void build_open_done(void *user)
{
    (void)user;
    if (s_list == NULL || s_open_count > 0) return;

    lvgl_port_lock(0);
    fw_ui_list_hint(s_list, "没有文本文件");
    lvgl_port_unlock();
}

/* 递归收集文本文件路径（层数有限；项数到上限 ED_LIST_MAX 就停）。
 * 只收集不建控件：列表交给分段构建 */
static void scan_dir(const char *dir, int depth, int *shown)
{
    if (depth > ED_SCAN_DEPTH || *shown >= ED_LIST_MAX) return;

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) return;

    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL && *shown < ED_LIST_MAX) {
        if (entry->is_dir) {
            if (entry->name[0] == '.') continue;    /* 隐藏目录 / 系统目录 */

            char sub[ED_FILE_MAX];
            if (snprintf(sub, sizeof(sub), "%s/%s", dir, entry->name) >= (int)sizeof(sub)) continue;
            scan_dir(sub, depth + 1, shown);
            continue;
        }

        if (!is_text_name(entry->name)) continue;
        if (!list_reserve((size_t)*shown + 1)) break;

        char full[ED_FILE_MAX];
        if (snprintf(full, sizeof(full), "%s/%s", dir, entry->name) >= (int)sizeof(full)) continue;

        strlcpy(s_paths[*shown], full, ED_FILE_MAX);
        (*shown)++;
    }

    svc_storage_iter_end(it);
}

static void open_cb(lv_event_t *e)
{
    (void)e;

    /* 已经开着就收起来（工具栏按钮当成开关） */
    if (s_open_page != NULL) {
        close_open_page();
        return;
    }

    if (s_paths == NULL) {
        list_reserve(ED_LIST_INIT);
    }
    if (s_paths == NULL) {
        fw_ui_toast("内存不足", 2000);
        return;
    }

    lvgl_port_lock(0);

    s_open_page = lv_obj_create(s_root);
    lv_obj_set_size(s_open_page, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H);
    lv_obj_set_pos(s_open_page, 0, FW_STATUSBAR_H);
    lv_obj_set_scrollable(s_open_page, false);
    lv_obj_set_style_bg_color(s_open_page, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(s_open_page, 12, 0);
    lv_obj_set_style_pad_row(s_open_page, 8, 0);
    lv_obj_set_style_border_width(s_open_page, 0, 0);
    lv_obj_set_flex_flow(s_open_page, LV_FLEX_FLOW_COLUMN);

    /* 标题行（关闭走状态栏返回键 / BOOT 键，页面上不放返回按钮） */
    lv_obj_t *title = lv_label_create(s_open_page);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "打开文本文件");
    lv_obj_set_style_text_font(title, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);

    s_list = fw_ui_list(s_open_page, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    lvgl_port_unlock();

    int shown = 0;
    scan_dir("/sdcard", 1, &shown);
    scan_dir("/internal", 1, &shown);
    s_open_count = (size_t)shown;

    /* 文件可能很多：分段建列表（建完在 build_open_done 里补空提示） */
    fw_ui_stage_start(s_stage, s_open_count, ED_BUILD_CHUNK, build_open_row, build_open_done, NULL);

    ESP_LOGI(TAG, "open page: %d file(s)", shown);
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *editor_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 顶部：文件名 + 打开 / 保存 / 新建 */
    s_bar = lv_obj_create(body);
    lv_obj_set_size(s_bar, lv_pct(100), 28);
    lv_obj_set_scrollable(s_bar, false);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_pad_column(s_bar, 8, 0);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_name = lv_label_create(s_bar);
    lv_obj_set_flex_grow(s_name, 1);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_name, fw_theme_color_text_secondary(), 0);

    fw_ui_icon_btn(s_bar, &icon_ui_folder, NULL, 32, open_cb, NULL);
    fw_ui_icon_btn(s_bar, &icon_ui_save, NULL, 32, save_cb, NULL);
    fw_ui_icon_btn(s_bar, &icon_ui_file, NULL, 32, new_cb, NULL);

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
    lv_obj_add_event_cb(s_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    /* 软键盘：贴屏幕底部（四周留 8，和文件管理 / Wi-Fi 输入浮层一致）、默认隐藏；
     * 焦点在 keyboard_show 里接上 */
    s_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_kb, lv_display_get_horizontal_resolution(lv_display_get_default()) - 2 * ED_KB_MARGIN,
                    ED_KB_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, -ED_KB_MARGIN);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_secondary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_card(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, fw_theme_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_kb, fw_theme_color_border(), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_kb, true);

    /* 换主题重建 / 首次创建：由 open_initial() 决定编辑区显示什么 */
    open_initial();

    if (s_stage == NULL) {
        s_stage = fw_ui_stage_create();
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* 重新进前台：文件管理可能带了新的 path=（点同一个文件每次都会重新打开） */
static void editor_on_resume(void *ctx)
{
    (void)ctx;
    (void)open_external();
}

/* 前一次 on_create 之后被 on_destroy 拆掉了界面：下一次 on_create 是"原地重建"，
 * 不是新打开一个文件，此时不能再去看（可能已经过期的）启动参数 */
static void editor_on_destroy(void *ctx)
{
    (void)ctx;
    s_rebuilding = true;

    /* 先停分段构建：回调会用到下面要删的列表 */
    fw_ui_stage_stop(s_stage);

    /* 未保存的改动先存回文件：换主题重建时 on_create 会再读回来，内容不丢 */
    lvgl_port_lock(0);
    if (s_dirty && s_file[0] != '\0' && s_ta != NULL) {
        const char *text = lv_textarea_get_text(s_ta);
        if (text != NULL && svc_storage_write(s_file, text, strlen(text)) == ESP_OK) {
            ESP_LOGI(TAG, "autosaved %s", s_file);
        } else {
            ESP_LOGW(TAG, "autosave %s failed", s_file);
        }
    }
    if (s_open_page != NULL) {
        lv_obj_delete(s_open_page);
        s_open_page = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_bar = NULL;
    s_ta = NULL;
    s_kb = NULL;
    s_name = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    free(s_paths);
    s_paths = NULL;
    s_paths_cap = 0;
    s_open_count = 0;
    fw_ui_stage_destroy(s_stage);
    s_stage = NULL;
}

/* 返回键：先收键盘，再关"打开"页，都没有才退出 */
static bool editor_on_back(void *ctx)
{
    (void)ctx;

    if (s_kb != NULL && !lv_obj_is_hidden(s_kb)) {
        keyboard_show(false);
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
    .icon = &icon_home_editor,
    .on_create = editor_on_create,
    .on_resume = editor_on_resume,
    .on_destroy = editor_on_destroy,
    .on_back = editor_on_back,
};
