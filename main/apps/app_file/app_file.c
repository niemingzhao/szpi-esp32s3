/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - File（文件管理）
 *
 * 完整的文件管理器，浏览 TF 卡（/sdcard）与内置 SPIFFS（/internal）：
 *   头部：位置按钮（显示当前路径，点开选存储并看容量）+ 上级 + 新建文件夹 + 更多
 *   列表：文件夹排在文件前面，图标区分文件夹 / 文本 / 音频 / 图片 / 其它，文件右侧显示大小
 *   点条目：文件夹进入；文件交给关联 App（音乐 / 图片 / 编辑器）
 *   长按条目：详情 / 重命名 / 复制 / 剪切 / 删除
 *   更多菜单：刷新 / 排序（名称 / 大小 / 类型）/ 粘贴到当前目录 / 格式化
 *
 * 删除、粘贴（复制 / 移动）、格式化都在独立任务里做（不冻界面），完成后用 lv_async_call
 * 回到 LVGL 任务刷新；涉及内置 SPIFFS（flash 擦写）时先摘掉看门狗自动重启。
 *
 * 目录项放堆上、按目录内容成倍扩（不在 App 里放大静态数组），一个目录有多少条目
 * 就列多少（上限 ROW_CAP_MAX 仅作防御）。
 */

#include "app_file.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "app.file";

#define PATH_MAX_LEN   200      /* 路径缓冲 */
#define NAME_MAX_LEN   96       /* 文件名 / 一行文本 */
#define ROW_INIT       64       /* 目录项数组起始容量（按需成倍扩） */
#define ROW_CAP_MAX    1024     /* 上限：再多界面也放不下，防御性拦截 */
#define OP_STACK       8192     /* 复制 / 删除任务的栈（递归复制要留够） */
#define KB             1024u
#define MB             (1024u * KB)
#define GB             (1024u * MB)

static const char *const SD_ROOT = "/sdcard";
static const char *const IN_ROOT = "/internal";

/* 排序方式 */
typedef enum { SORT_NAME = 0, SORT_SIZE, SORT_TYPE } sort_t;

/* 文件类别（决定图标与"类型"文案） */
typedef enum { KIND_DIR = 0, KIND_TEXT, KIND_AUDIO, KIND_IMAGE, KIND_OTHER } kind_t;

/* 一行 = 一个目录项 */
typedef struct {
    char name[NAME_MAX_LEN];
    size_t size;
    bool is_dir;
} row_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_path_btn = NULL;     /* 头部位置按钮（图标 + 当前路径） */
static lv_obj_t *s_newdir_btn = NULL;   /* 新建文件夹（内置存储不支持，藏起来） */
static lv_obj_t *s_list = NULL;
static char s_cur[PATH_MAX_LEN] = "/sdcard";
static row_t *s_rows = NULL;            /* 堆上分配，容量按目录内容成倍扩 */
static size_t s_row_cap = 0;
static size_t s_row_count = 0;
static bool s_truncated = false;
static char s_scan_dir[PATH_MAX_LEN] = "";   /* 上一次扫描的目录：同目录重扫时保持滚动位置 */
static fw_ui_stage_t *s_stage = NULL;        /* 列表分段构建 */
static sort_t s_sort = SORT_NAME;

/* 剪贴板：复制 / 剪切一项，粘贴到当前目录 */
static char s_clip[PATH_MAX_LEN] = "";
static char s_clip_name[NAME_MAX_LEN] = "";
static bool s_clip_cut = false;
static bool s_clip_dir = false;

/* 当前被长按的条目（操作菜单用） */
static char s_act_path[PATH_MAX_LEN] = "";
static char s_act_name[NAME_MAX_LEN] = "";
static bool s_act_dir = false;

/* 菜单浮层（更多 / 位置 / 条目操作 三种用途共用） */
static lv_obj_t *s_menu = NULL;
static lv_obj_t *s_menu_list = NULL;

/* 输入浮层（新建文件夹 / 重命名） */
typedef enum { INPUT_NEW_DIR = 0, INPUT_RENAME } input_mode_t;
static lv_obj_t *s_input = NULL;
static lv_obj_t *s_input_ta = NULL;
static lv_obj_t *s_input_kb = NULL;
static input_mode_t s_input_mode = INPUT_NEW_DIR;
static char s_input_path[PATH_MAX_LEN] = "";

/* 后台操作（删除 / 粘贴 / 格式化） */
typedef enum { OP_NONE = 0, OP_DELETE, OP_PASTE, OP_FORMAT } op_t;
static volatile bool s_op_busy = false;
static op_t s_op = OP_NONE;
static bool s_op_ok = false;
static bool s_op_cut = false;
static bool s_op_wdt_off = false;
static svc_storage_type_t s_op_type = SVC_STORAGE_INTERNAL_FLASH;
static char s_op_from[PATH_MAX_LEN] = "";
static char s_op_to[PATH_MAX_LEN] = "";

/* 前置声明 */
static void scan(void);
static void menu_open(const char *title);
static lv_obj_t *menu_add(const char *text, const char *value, lv_event_cb_t cb, void *user);
static void menu_close(void);
static void input_close(void);

/* ------------------------------- 小工具 ------------------------------- */

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return (dot != NULL) && (strcasecmp(dot, ext) == 0);
}

/* 路径在 TF 卡上？两个存储前缀互斥，所以这样判断足够 */
static bool on_tf(const char *path)
{
    return strncmp(path, SD_ROOT, strlen(SD_ROOT)) == 0;
}

static bool on_internal(const char *path)
{
    return path[0] != '\0' && !on_tf(path);
}

static bool same_storage(const char *a, const char *b)
{
    return on_tf(a) == on_tf(b);
}

static svc_storage_type_t storage_of(const char *path)
{
    return on_tf(path) ? SVC_STORAGE_TF_CARD : SVC_STORAGE_INTERNAL_FLASH;
}

static kind_t kind_of(const char *name, bool is_dir)
{
    if (is_dir) return KIND_DIR;

    /* 文本类扩展名和 app_editor 的 is_text_name() 保持一致（脚本也当文本） */
    if (has_ext(name, ".txt") || has_ext(name, ".md") || has_ext(name, ".markdown") ||
        has_ext(name, ".log") || has_ext(name, ".csv") || has_ext(name, ".json") ||
        has_ext(name, ".ini") || has_ext(name, ".cfg") || has_ext(name, ".conf") ||
        has_ext(name, ".toml") || has_ext(name, ".xml") || has_ext(name, ".yml") ||
        has_ext(name, ".yaml") || has_ext(name, ".html") || has_ext(name, ".css") ||
        has_ext(name, ".js") || has_ext(name, ".py") || has_ext(name, ".lua") ||
        has_ext(name, ".sh") || has_ext(name, ".bat") || has_ext(name, ".ps1") ||
        has_ext(name, ".c") || has_ext(name, ".h") || has_ext(name, ".cpp") ||
        has_ext(name, ".hpp") || has_ext(name, ".ino")) {
        return KIND_TEXT;
    }
    if (has_ext(name, ".mp3") || has_ext(name, ".wav")) return KIND_AUDIO;
    if (has_ext(name, ".png") || has_ext(name, ".jpg") || has_ext(name, ".jpeg") ||
        has_ext(name, ".gif") || has_ext(name, ".bmp")) {
        return KIND_IMAGE;
    }
    return KIND_OTHER;
}

static const lv_image_dsc_t *kind_icon(kind_t k)
{
    switch (k) {
    case KIND_DIR:   return &icon_ui_folder;
    case KIND_TEXT:  return &icon_ui_file_text;
    case KIND_AUDIO: return &icon_ui_file_audio;
    case KIND_IMAGE: return &icon_ui_file_image;
    default:         return &icon_ui_file;
    }
}

static const char *kind_text(kind_t k)
{
    switch (k) {
    case KIND_DIR:   return "文件夹";
    case KIND_TEXT:  return "文本";
    case KIND_AUDIO: return "音频";
    case KIND_IMAGE: return "图片";
    default:         return "文件";
    }
}

static void human_size(uint64_t bytes, char *out, size_t len)
{
    if (bytes < KB) {
        snprintf(out, len, "%u B", (unsigned)bytes);
    } else if (bytes < MB) {
        snprintf(out, len, "%.1f KB", (double)bytes / KB);
    } else if (bytes < GB) {
        snprintf(out, len, "%.1f MB", (double)bytes / MB);
    } else {
        snprintf(out, len, "%.2f GB", (double)bytes / GB);
    }
}

/* 文件名合法性：非空、不含路径分隔符与 FAT 非法字符、不是 . / ..、长度够 */
static bool name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    if (strlen(name) >= NAME_MAX_LEN) return false;

    for (const char *p = name; *p != '\0'; p++) {
        if (strchr("/\\:*?\"<>|", *p) != NULL) return false;
    }
    return true;
}

/* path 是否就是 dir 或 dir 的子路径（用于拦住"把目录粘进它自己"） */
static bool is_under(const char *path, const char *dir)
{
    const size_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0) return false;
    return (path[n] == '\0') || (path[n] == '/');
}

/* 取 path 所在目录（写回 buf） */
static void dir_of(const char *path, char *out, size_t len)
{
    snprintf(out, len, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash != NULL && slash != out) *slash = '\0';
}

/* 拼出 dir/name（越界按截断处理）。不用 snprintf：GCC 15 的 -Werror=format-truncation
 * 对"已知长度数组 + 已知长度数组"的调用会直接判成错误（两个缓冲都够大才算得出来） */
static void join_path(char *dst, size_t len, const char *dir, const char *name)
{
    if (len == 0) return;

    size_t n = strlen(dir);
    if (n > len - 1) n = len - 1;
    memcpy(dst, dir, n);

    if (n + 1 < len) {
        dst[n++] = '/';
        strlcpy(dst + n, name, len - n);
    } else {
        dst[n] = '\0';
    }
}

/* 保证目录项数组至少能放 need 行；不够就成倍扩（上限 ROW_CAP_MAX）。
 * 目录里有多少条目事先不知道，固定容量要么浪费要么列不全 */
static bool rows_reserve(size_t need)
{
    if (need <= s_row_cap) return true;
    if (need > ROW_CAP_MAX) return false;

    size_t cap = (s_row_cap > 0) ? s_row_cap : ROW_INIT;
    while (cap < need) cap *= 2;
    if (cap > ROW_CAP_MAX) cap = ROW_CAP_MAX;

    row_t *p = realloc(s_rows, cap * sizeof(row_t));
    if (p == NULL) return false;

    s_rows = p;
    s_row_cap = cap;
    return true;
}

/* 目标不重名：name、name (2)、name (3) …；找不到可用名字（同名的太多）返回 false */
static bool unique_path(const char *dir, const char *name, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", dir, name);
    if (svc_storage_exists(out, NULL) != ESP_OK) return true;

    char base[NAME_MAX_LEN];
    const char *dot = strrchr(name, '.');
    const char *ext = "";
    if (dot != NULL && dot != name) {
        size_t n = (size_t)(dot - name);
        if (n >= sizeof(base)) n = sizeof(base) - 1;
        memcpy(base, name, n);
        base[n] = '\0';
        ext = dot;
    } else {
        snprintf(base, sizeof(base), "%s", name);
    }

    for (int i = 2; i < 1000; i++) {
        snprintf(out, len, "%s/%s (%d)%s", dir, base, i, ext);
        if (svc_storage_exists(out, NULL) != ESP_OK) return true;
    }
    return false;
}

static bool at_storage_root(void)
{
    return (strcmp(s_cur, SD_ROOT) == 0) || (strcmp(s_cur, IN_ROOT) == 0);
}

static void go_up(void)
{
    if (at_storage_root()) return;

    char *slash = strrchr(s_cur, '/');
    if (slash != NULL && slash != s_cur) *slash = '\0';
    scan();
}

static void enter_dir(const char *path)
{
    strlcpy(s_cur, path, sizeof(s_cur));
    scan();
}

/* ------------------------------- 列表 ------------------------------- */

/* 目录恒在前；同一类里按当前排序方式 */
static int row_cmp(const void *a, const void *b)
{
    const row_t *x = (const row_t *)a;
    const row_t *y = (const row_t *)b;

    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;

    if (s_sort == SORT_SIZE && x->size != y->size) {
        return (x->size > y->size) ? -1 : 1;        /* 大文件在前 */
    }
    if (s_sort == SORT_TYPE) {
        const int kx = (int)kind_of(x->name, x->is_dir);
        const int ky = (int)kind_of(y->name, y->is_dir);
        if (kx != ky) return kx - ky;
    }
    return strcasecmp(x->name, y->name);
}

static void row_cb(lv_event_t *e);
static void act_menu_open_cb(lv_event_t *e);

#define FILE_BUILD_CHUNK  12    /* 分段构建：每批建几行 */

static int32_t s_keep_y = 0;             /* 重扫时要保持的滚动位置 */

/* 分段建第 idx 行（由 fw_ui_stage_start 调，已在 LVGL 锁内） */
static void build_row(size_t idx, void *user)
{
    (void)user;
    if (s_list == NULL || idx >= s_row_count) return;

    char value[16] = "";
    const kind_t k = kind_of(s_rows[idx].name, s_rows[idx].is_dir);

    /* 文件夹不显示大小（它的大小是 0，显示出来反而误导） */
    if (k != KIND_DIR) human_size(s_rows[idx].size, value, sizeof(value));

    lv_obj_t *item = fw_ui_list_add_icon(s_list, kind_icon(k), s_rows[idx].name,
                                         (value[0] != '\0') ? value : NULL,
                                         row_cb, (void *)(uintptr_t)idx);
    if (item == NULL) return;

    /* 长按 = 条目操作菜单（点 = 进入 / 打开） */
    lv_obj_add_event_cb(item, act_menu_open_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)idx);

    /* 剪贴板里那一项标出来：知道复制的是谁、待移动的是谁 */
    if (s_clip[0] != '\0') {
        char full[PATH_MAX_LEN];
        join_path(full, sizeof(full), s_cur, s_rows[idx].name);
        if (strcmp(full, s_clip) == 0) {
            fw_ui_list_mark(item, true);
            fw_ui_list_value_color(item, fw_theme_color_accent());
        }
    }
}

/* 全部建完：空目录 / 截断提示 + 恢复滚动位置 */
static void build_done(void *user)
{
    (void)user;
    if (s_list == NULL) return;

    lvgl_port_lock(0);

    if (s_row_count == 0) {
        /* 目录可能已经没了（拔卡 / 被删）：给个明确点的提示，别只写"空目录" */
        if (svc_storage_exists(s_cur, NULL) != ESP_OK) {
            fw_ui_list_hint(s_list, "存储未挂载或目录已不存在");
        } else {
            fw_ui_list_hint(s_list, "空目录");
        }
    } else {
        if (s_truncated) fw_ui_list_hint(s_list, "条目过多，只显示前 1024 项");
        if (s_keep_y > 0) {
            lv_obj_update_layout(s_list);
            lv_obj_scroll_to_y(s_list, s_keep_y, LV_ANIM_OFF);
        }
    }

    lvgl_port_unlock();
}

static void scan(void)
{
    s_row_count = 0;
    s_truncated = false;

    /* 目录遍历不持 LVGL 锁：大目录读卡可能几百毫秒，锁着会让别处
     * lvgl_port_lock(超时) 的刷新全部超时丢掉 */
    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(s_cur, &it) == ESP_OK) {
        svc_storage_entry_t *entry;
        while ((entry = svc_storage_iter_next(it)) != NULL) {
            if (s_row_count >= ROW_CAP_MAX) {
                s_truncated = true;
                break;
            }
            /* 名字比列表行缓冲还长的条目跳过：截断后拼出来的路径会指向别的文件 */
            if (strlen(entry->name) >= NAME_MAX_LEN) continue;
            /* 拼出来的完整路径放不下的也跳过：同理，截断后进的 / 开的会是别的条目 */
            if (strlen(s_cur) + 1 + strlen(entry->name) >= PATH_MAX_LEN) continue;
            if (!rows_reserve(s_row_count + 1)) {
                s_truncated = true;
                break;
            }

            row_t *r = &s_rows[s_row_count++];
            strlcpy(r->name, entry->name, sizeof(r->name));
            r->is_dir = entry->is_dir;
            r->size = entry->size;
        }
        svc_storage_iter_end(it);
    }

    if (s_row_count > 1) qsort(s_rows, s_row_count, sizeof(s_rows[0]), row_cmp);

    /* 同一个目录重扫（刷新 / 操作完成 / 回前台）时保持原来的滚动位置 */
    const bool same_dir = (strcmp(s_scan_dir, s_cur) == 0);
    strlcpy(s_scan_dir, s_cur, sizeof(s_scan_dir));

    lvgl_port_lock(0);

    s_keep_y = (same_dir && s_list != NULL) ? lv_obj_get_scroll_y(s_list) : 0;

    /* 头部：当前位置（位置按钮里的文字） */
    if (s_path_btn != NULL) {
        lv_obj_t *lb = lv_obj_get_child(s_path_btn, 1);     /* 图标 + 文字 */
        if (lb != NULL) lv_label_set_text(lb, s_cur);
    }

    /* SPIFFS 的 VFS 没有 mkdir：内置存储里不显示「新建文件夹」 */
    if (s_newdir_btn != NULL) lv_obj_set_hidden(s_newdir_btn, on_internal(s_cur));

    if (s_list != NULL) {
        lv_obj_clean(s_list);
        /* 条目多时分段建，避免一次把 LVGL 任务（连带触摸）卡住几百毫秒 */
        fw_ui_stage_start(s_stage, s_row_count, FILE_BUILD_CHUNK, build_row, build_done, NULL);
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "%s: %u entr(ies), sort=%d", s_cur, (unsigned)s_row_count, (int)s_sort);
}

/* ------------------------------ 打开文件 ------------------------------ */

static void open_row(const row_t *r)
{
    char path[PATH_MAX_LEN];
    join_path(path, sizeof(path), s_cur, r->name);

    /* 三个"内容 App"都支持 "App?path=/xxx"：直接打开这个文件（音频还会直接开始播） */
    const char *app = NULL;
    switch (kind_of(r->name, false)) {
    case KIND_AUDIO: app = "Music"; break;
    case KIND_IMAGE: app = "Image"; break;
    case KIND_TEXT:  app = "Editor"; break;
    default:         break;
    }

    if (app == NULL) {
        fw_ui_toast("没有关联的应用，请在电脑上打开", 2500);
        return;
    }

    char uri[PATH_MAX_LEN + 16];
    snprintf(uri, sizeof(uri), "%s?path=%s", app, path);
    if (fw_app_mgr_launch_uri(uri) != ESP_OK) fw_ui_toast("启动失败", 1500);
}

static void row_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_row_count) return;

    if (s_rows[idx].is_dir) {
        char path[PATH_MAX_LEN];
        join_path(path, sizeof(path), s_cur, s_rows[idx].name);
        enter_dir(path);
        return;
    }
    open_row(&s_rows[idx]);
}

/* ------------------------------ 后台操作 ------------------------------ */

static void op_done_async(void *arg)
{
    (void)arg;

    lvgl_port_lock(0);

    const char *msg = NULL;
    if (s_op == OP_DELETE) {
        msg = s_op_ok ? "已删除" : "删除失败";
    } else if (s_op == OP_PASTE) {
        msg = s_op_ok ? (s_op_cut ? "已移动" : "已复制") : "操作失败";
    } else if (s_op == OP_FORMAT) {
        msg = s_op_ok ? "格式化完成" : "格式化失败";
        /* 成功才回到该存储的根目录（失败时当前目录还是有效的，别乱跳） */
        if (s_op_ok) {
            strlcpy(s_cur, (s_op_type == SVC_STORAGE_TF_CARD) ? SD_ROOT : IN_ROOT, sizeof(s_cur));
        }
    }
    if (msg != NULL) fw_ui_toast(msg, 1800);

    /* 剪切成功就把剪贴板清掉（复制保留，可以连着粘多次） */
    if (s_op == OP_PASTE && s_op_ok && s_op_cut) {
        s_clip[0] = '\0';
        s_clip_name[0] = '\0';
        s_clip_cut = false;
    }

    scan();

    lvgl_port_unlock();

    if (s_op_wdt_off) {
        svc_watchdog_arm();
        s_op_wdt_off = false;
    }
    s_op_busy = false;
    s_op = OP_NONE;
}

/* 同一个存储内剪切 = rename（瞬间完成，不搬数据）；其余情况复制，剪切再删源 */
static bool do_paste(void)
{
    if (s_op_cut && same_storage(s_op_from, s_op_to)) {
        return (svc_storage_rename(s_op_from, s_op_to) == ESP_OK);
    }

    if (svc_storage_copy(s_op_from, s_op_to) != ESP_OK) return false;
    if (!s_op_cut) return true;
    return (svc_storage_remove_tree(s_op_from) == ESP_OK);
}

static void op_task(void *arg)
{
    (void)arg;

    switch (s_op) {
    case OP_DELETE:
        s_op_ok = (svc_storage_remove_tree(s_op_from) == ESP_OK);
        break;
    case OP_FORMAT:
        s_op_ok = (svc_storage_format(s_op_type) == ESP_OK);
        break;
    case OP_PASTE:
        s_op_ok = do_paste();
        break;
    default:
        break;
    }

    if (lvgl_port_lock(0)) {
        lv_async_call(op_done_async, NULL);
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

/* 擦写内置 SPIFFS 期间别的任务可能长时间拿不到 flash 而喂不了狗，先摘掉自动重启 */
static bool op_needs_wdt_off(void)
{
    if (s_op == OP_FORMAT) return true;
    return on_internal(s_op_from) || on_internal(s_op_to);
}

static void op_start(op_t op)
{
    if (s_op_busy) {
        fw_ui_toast("正在处理，请稍候", 1500);
        return;
    }

    s_op = op;
    s_op_ok = false;
    s_op_busy = true;

    if (op_needs_wdt_off()) {
        svc_watchdog_disarm();
        s_op_wdt_off = true;
    }

    if (xTaskCreate(op_task, "app_file_op", OP_STACK, NULL, 4, NULL) != pdPASS) {
        s_op_busy = false;
        s_op = OP_NONE;
        if (s_op_wdt_off) {
            svc_watchdog_arm();
            s_op_wdt_off = false;
        }
        fw_ui_toast("操作失败", 1800);
    }
}

/* ------------------------------ 菜单浮层 ------------------------------ */

static void menu_close_cb(lv_event_t *e)
{
    (void)e;
    menu_close();
}

static void menu_open(const char *title)
{
    if (s_menu != NULL) return;
    if (s_root == NULL) return;

    s_menu = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_menu, 0, 0);
    lv_obj_set_style_bg_color(s_menu, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_menu, 12, 0);
    lv_obj_set_style_pad_top(s_menu, FW_STATUSBAR_H + 12, 0);
    lv_obj_set_style_pad_row(s_menu, 8, 0);
    lv_obj_set_scrollable(s_menu, false);
    lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_menu, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 第一行：标题（撑满）+ 关闭 */
    lv_obj_t *row = lv_obj_create(s_menu);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lb = lv_label_create(row);
    lv_obj_set_flex_grow(lb, 1);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(lb, title != NULL ? title : "");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);

    fw_ui_btn(row, "关闭", 52, false, menu_close_cb, NULL);

    s_menu_list = fw_ui_list(s_menu, NULL);
    lv_obj_set_width(s_menu_list, lv_pct(100));
    lv_obj_set_flex_grow(s_menu_list, 1);
}

/* 加一行菜单项（返回项，便于标记当前值） */
static lv_obj_t *menu_add(const char *text, const char *value, lv_event_cb_t cb, void *user)
{
    if (s_menu_list == NULL) return NULL;

    if (value != NULL && value[0] != '\0') {
        return fw_ui_list_add_value(s_menu_list, text, value, cb, user);
    }
    return fw_ui_list_add(s_menu_list, text, cb, user);
}

static void menu_close(void)
{
    if (s_menu == NULL) return;

    lv_obj_delete(s_menu);
    s_menu = NULL;
    s_menu_list = NULL;
}

/* ------------------------------ 位置选择 ------------------------------ */

static void loc_pick_cb(lv_event_t *e)
{
    const svc_storage_type_t type =
        (svc_storage_type_t)(intptr_t)lv_event_get_user_data(e);

    menu_close();
    enter_dir((type == SVC_STORAGE_TF_CARD) ? SD_ROOT : IN_ROOT);
}

static lv_obj_t *loc_add_row(const char *label, svc_storage_type_t type)
{
    char value[32] = "未挂载";
    svc_storage_info_t info;

    if (svc_storage_get_info(type, &info) == ESP_OK) {
        char freeb[16];
        human_size(info.free_bytes, freeb, sizeof(freeb));
        snprintf(value, sizeof(value), "可用 %s", freeb);
    }

    lv_obj_t *item = menu_add(label, value, loc_pick_cb, (void *)(intptr_t)type);
    if (item != NULL && storage_of(s_cur) == type) fw_ui_list_mark(item, true);
    return item;
}

static void loc_cb(lv_event_t *e)
{
    (void)e;

    menu_open("存储位置");
    loc_add_row("TF 卡", SVC_STORAGE_TF_CARD);
    loc_add_row("内置存储", SVC_STORAGE_INTERNAL_FLASH);
}

/* ------------------------------ 条目操作 ------------------------------ */

static void act_info_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    /* 位置可能很长：这里 4 段加起来要放得下（名称 96 + 路径 200 + 其余） */
    char msg[NAME_MAX_LEN + PATH_MAX_LEN + 96];
    char size[16];

    if (s_act_dir) {
        snprintf(size, sizeof(size), "—");
    } else {
        size_t bytes = 0;
        svc_storage_exists(s_act_path, &bytes);
        human_size(bytes, size, sizeof(size));
    }

    /* 名称与位置各占一行，类型 / 大小短，合成一行：行数少、正文短，对话框不用撑太高 */
    snprintf(msg, sizeof(msg), "%s\n%s · %s\n%s",
             s_act_name, kind_text(kind_of(s_act_name, s_act_dir)), size, s_act_path);

    fw_ui_dialog(NULL, "详细信息", msg, FW_DIALOG_BTN_OK, NULL, NULL);
}

static void act_copy_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    strlcpy(s_clip, s_act_path, sizeof(s_clip));
    strlcpy(s_clip_name, s_act_name, sizeof(s_clip_name));
    s_clip_cut = false;
    s_clip_dir = s_act_dir;

    char msg[NAME_MAX_LEN + 16];
    snprintf(msg, sizeof(msg), "已复制 %s", s_clip_name);
    fw_ui_toast(msg, 1800);
    scan();                     /* 把剪贴板那一项标出来 */
}

static void act_cut_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    strlcpy(s_clip, s_act_path, sizeof(s_clip));
    strlcpy(s_clip_name, s_act_name, sizeof(s_clip_name));
    s_clip_cut = true;
    s_clip_dir = s_act_dir;

    char msg[NAME_MAX_LEN + 16];
    snprintf(msg, sizeof(msg), "已剪切 %s", s_clip_name);
    fw_ui_toast(msg, 1800);
    scan();
}

static void delete_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    strlcpy(s_op_from, s_act_path, sizeof(s_op_from));
    op_start(OP_DELETE);
}

static void act_delete_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    char msg[160];
    if (s_act_dir) {
        snprintf(msg, sizeof(msg), "删除「%s」及其中全部内容？删除后无法恢复。", s_act_name);
    } else {
        snprintf(msg, sizeof(msg), "删除「%s」？删除后无法恢复。", s_act_name);
    }

    fw_ui_dialog(NULL, "删除", msg, FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL,
                 delete_confirm_cb, NULL);
}

static void input_open(input_mode_t mode, const char *path, const char *name);

static void act_rename_cb(lv_event_t *e)
{
    (void)e;
    menu_close();
    input_open(INPUT_RENAME, s_act_path, s_act_name);
}

static void act_menu_open_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_row_count) return;

    join_path(s_act_path, sizeof(s_act_path), s_cur, s_rows[idx].name);
    snprintf(s_act_name, sizeof(s_act_name), "%s", s_rows[idx].name);
    s_act_dir = s_rows[idx].is_dir;

    menu_open(s_act_name);
    menu_add("详细信息", NULL, act_info_cb, NULL);
    menu_add("重命名", NULL, act_rename_cb, NULL);
    menu_add("复制", NULL, act_copy_cb, NULL);
    menu_add("剪切", NULL, act_cut_cb, NULL);
    menu_add("删除", NULL, act_delete_cb, NULL);
}

/* ------------------------------ 更多菜单 ------------------------------ */

static void paste_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    if (s_clip[0] == '\0') return;

    /* 剪切到原目录等于原地不动 */
    char src_dir[PATH_MAX_LEN];
    dir_of(s_clip, src_dir, sizeof(src_dir));
    if (s_clip_cut && strcmp(src_dir, s_cur) == 0) {
        fw_ui_toast("源和目标在同一目录", 1800);
        return;
    }

    /* 目录不能粘进它自己或它的子目录（否则会无限递归复制） */
    if (s_clip_dir && is_under(s_cur, s_clip)) {
        fw_ui_toast("不能粘贴到自身或子目录", 2000);
        return;
    }

    if (!unique_path(s_cur, s_clip_name, s_op_to, sizeof(s_op_to))) {
        fw_ui_toast("同名文件太多，换个名字再试", 2000);
        return;
    }
    strlcpy(s_op_from, s_clip, sizeof(s_op_from));
    s_op_cut = s_clip_cut;
    op_start(OP_PASTE);
}

static void sort_cb(lv_event_t *e)
{
    const sort_t next = (sort_t)(((int)s_sort + 1) % 3);
    (void)e;

    s_sort = next;
    menu_close();

    const char *msg = (s_sort == SORT_NAME) ? "按名称排序"
                    : (s_sort == SORT_SIZE) ? "按大小排序" : "按类型排序";
    fw_ui_toast(msg, 1500);
    scan();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    menu_close();
    scan();
    fw_ui_toast("已刷新", 1200);
}

static void format_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;
    op_start(OP_FORMAT);
}

static void format_tf_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    svc_storage_info_t info;
    if (svc_storage_get_info(SVC_STORAGE_TF_CARD, &info) != ESP_OK) {
        fw_ui_toast("TF 卡未挂载", 2000);
        return;
    }

    s_op_type = SVC_STORAGE_TF_CARD;
    fw_ui_dialog(NULL, "格式化 TF 卡", "将清空 TF 卡全部数据，且无法恢复，确定继续？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, format_confirm_cb, NULL);
}

static void format_internal_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    s_op_type = SVC_STORAGE_INTERNAL_FLASH;
    fw_ui_dialog(NULL, "格式化内置存储", "将清空内置存储全部数据，且无法恢复，确定继续？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, format_confirm_cb, NULL);
}

static void more_cb(lv_event_t *e)
{
    (void)e;

    menu_open("文件管理");

    menu_add("刷新", NULL, refresh_cb, NULL);

    const char *sorts = (s_sort == SORT_NAME) ? "名称"
                      : (s_sort == SORT_SIZE) ? "大小" : "类型";
    menu_add("排序", sorts, sort_cb, NULL);

    if (s_clip[0] != '\0') {
        char value[NAME_MAX_LEN + 16];
        snprintf(value, sizeof(value), "%s%s", s_clip_name, s_clip_cut ? "（剪切）" : "（复制）");
        menu_add("粘贴到当前目录", value, paste_cb, NULL);
    }

    menu_add("格式化 TF 卡", NULL, format_tf_cb, NULL);
    menu_add("格式化内置存储", NULL, format_internal_cb, NULL);
}

/* ------------------------------ 输入浮层 ------------------------------ */

static void input_submit(void)
{
    char name[NAME_MAX_LEN] = "";

    if (s_input_ta != NULL) {
        snprintf(name, sizeof(name), "%s", lv_textarea_get_text(s_input_ta));
    }

    if (!name_valid(name)) {
        fw_ui_toast("名称不能为空，且不能含 / \\ : * ? \" < > |", 2500);
        return;
    }

    if (s_input_mode == INPUT_NEW_DIR) {
        char path[PATH_MAX_LEN];
        join_path(path, sizeof(path), s_cur, name);

        if (svc_storage_exists(path, NULL) == ESP_OK) {
            fw_ui_toast("同名文件夹已存在", 1800);
            return;
        }
        if (svc_storage_mkdir(path) != ESP_OK) {
            fw_ui_toast("新建失败", 1800);
            return;
        }
        input_close();
        fw_ui_toast("已新建文件夹", 1500);
        scan();
        return;
    }

    /* 重命名：名字没变就直接收工 */
    char dir[PATH_MAX_LEN];
    dir_of(s_input_path, dir, sizeof(dir));

    const char *orig = strrchr(s_input_path, '/');
    orig = (orig != NULL) ? orig + 1 : s_input_path;
    if (strcmp(name, orig) == 0) {
        input_close();
        return;
    }

    char dst[PATH_MAX_LEN];
    join_path(dst, sizeof(dst), dir, name);

    esp_err_t err = svc_storage_rename(s_input_path, dst);
    input_close();

    if (err == ESP_ERR_INVALID_STATE) {
        fw_ui_toast("同名文件已存在", 1800);
    } else if (err != ESP_OK) {
        fw_ui_toast("重命名失败", 1800);
    } else {
        fw_ui_toast("已重命名", 1500);
    }
    scan();
}

static void input_kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        input_submit();
    } else if (code == LV_EVENT_CANCEL) {
        input_close();
    }
}

static void input_cancel_cb(lv_event_t *e)
{
    (void)e;
    input_close();
}

static void input_open(input_mode_t mode, const char *path, const char *name)
{
    if (s_input != NULL) return;
    if (s_root == NULL) return;

    s_input_mode = mode;
    s_input_path[0] = '\0';
    if (path != NULL) strlcpy(s_input_path, path, sizeof(s_input_path));

    s_input = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input);
    lv_obj_set_size(s_input, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_input, 0, 0);
    lv_obj_set_style_bg_color(s_input, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_input, LV_OPA_COVER, 0);
    /* 和 Wi-Fi 密码 / 配对输码浮层同一版式：内边距 8、首行从状态栏下 8 开始，
     * 键盘高 120 贴底再留 8 —— 输入框（74~110）正好在键盘顶（112）之上 */
    lv_obj_set_style_pad_all(s_input, 8, 0);
    lv_obj_set_style_pad_top(s_input, FW_STATUSBAR_H + 8, 0);
    lv_obj_set_style_pad_row(s_input, 8, 0);
    lv_obj_set_scrollable(s_input, false);
    lv_obj_set_flex_flow(s_input, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_input, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 第一行：提示（撑满）+ 取消 */
    lv_obj_t *row = lv_obj_create(s_input);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hint = lv_label_create(row);
    lv_obj_set_flex_grow(hint, 1);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(hint, (mode == INPUT_NEW_DIR) ? "新建文件夹" : "重命名");
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);

    fw_ui_btn(row, "取消", 52, false, input_cancel_cb, NULL);

    /* 第二行：输入框（撑满），重命名时预填原名 */
    s_input_ta = fw_ui_textarea(s_input, "名称");
    lv_obj_set_width(s_input_ta, lv_pct(100));
    if (mode == INPUT_RENAME && name != NULL) {
        lv_textarea_set_text(s_input_ta, name);
    }

    /* 键盘：挂在根部，不进浮层的纵向布局；尺寸和 Wi-Fi / 天气浮层一致 */
    s_input_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_input_kb, lv_display_get_horizontal_resolution(lv_display_get_default()) - 16,
                    120);
    lv_obj_align(s_input_kb, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_keyboard_set_textarea(s_input_kb, s_input_ta);
    lv_obj_add_event_cb(s_input_kb, input_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_input_kb, input_kb_cb, LV_EVENT_CANCEL, NULL);
}

static void input_close(void)
{
    if (s_input_kb != NULL) {
        lv_obj_delete(s_input_kb);
        s_input_kb = NULL;
    }
    if (s_input != NULL) {
        lv_obj_delete(s_input);
        s_input = NULL;
        s_input_ta = NULL;
    }
}

static void new_dir_cb(lv_event_t *e)
{
    (void)e;
    input_open(INPUT_NEW_DIR, NULL, NULL);
}

/* ------------------------------ 头部 ------------------------------ */

static void up_cb(lv_event_t *e)
{
    (void)e;

    if (at_storage_root()) {
        fw_ui_toast("已经在根目录", 1500);
        return;
    }
    go_up();
}

static void *file_on_create(void)
{
    lvgl_port_lock(0);

    if (s_rows == NULL) {
        rows_reserve(ROW_INIT);
    }
    if (s_rows == NULL) {
        fw_ui_toast("内存不足", 2000);
    }

    if (s_stage == NULL) {
        s_stage = fw_ui_stage_create();
    }

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 文件列表要尽量多露几行：上下内边距收到 8、行距收到 4（放不下时收紧，内容优先） */
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_row(body, 4, 0);

    /* 头部：位置按钮（撑满）+ 上级 + 新建文件夹 + 更多 */
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), 28);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_path_btn = fw_ui_icon_btn(bar, &icon_ui_pin, s_cur, 0, loc_cb, NULL);
    fw_ui_icon_btn(bar, &icon_ui_up, NULL, 34, up_cb, NULL);
    s_newdir_btn = fw_ui_icon_btn(bar, &icon_ui_folder_plus, NULL, 34, new_dir_cb, NULL);
    fw_ui_icon_btn(bar, &icon_ui_more, NULL, 34, more_cb, NULL);

    s_list = fw_ui_list(body, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    lvgl_port_unlock();

    scan();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void file_on_destroy(void *ctx)
{
    (void)ctx;

    /* 停掉未建完的分段构建：下面的回调会用到已经被删的列表 */
    fw_ui_stage_stop(s_stage);

    lvgl_port_lock(0);

    /* 浮层先关掉（键盘挂在根屏上，和根屏一起删） */
    input_close();
    menu_close();

    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_path_btn = NULL;
    s_newdir_btn = NULL;
    s_list = NULL;

    lvgl_port_unlock();

    free(s_rows);
    s_rows = NULL;
    s_row_cap = 0;
    s_row_count = 0;
    s_scan_dir[0] = '\0';
    fw_ui_stage_destroy(s_stage);
    s_stage = NULL;
}

/* 进前台重扫一遍：别的 App（下载 / 录音 / 拍照）可能刚往存储里写了东西。
 * 只挂 on_resume：首次创建由 on_create 扫，返回栈回来 / 主页键回来都走 on_resume，
 * 两个都挂会让同一次进入扫两遍（大目录很贵） */
static void file_on_show(void *ctx)
{
    (void)ctx;
    scan();
}

/* 返回键：先关浮层，再回上一级目录，已在根目录才退出 App */
static bool file_on_back(void *ctx)
{
    (void)ctx;

    if (s_input != NULL) {
        lvgl_port_lock(0);
        input_close();
        lvgl_port_unlock();
        return true;
    }
    if (s_menu != NULL) {
        lvgl_port_lock(0);
        menu_close();
        lvgl_port_unlock();
        return true;
    }
    if (!at_storage_root()) {
        lvgl_port_lock(0);
        go_up();
        lvgl_port_unlock();
        return true;
    }
    return false;
}

const fw_app_desc_t app_file_desc = {
    .name = "File",
    .title = "文件管理",
    .icon = &icon_home_file,
    .on_create = file_on_create,
    .on_resume = file_on_show,
    .on_destroy = file_on_destroy,
    .on_back = file_on_back,
};
