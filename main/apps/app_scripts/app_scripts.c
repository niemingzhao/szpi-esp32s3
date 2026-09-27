/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Scripts（脚本）
 *
 * 列出 TF 卡与内置存储整盘 里的脚本（fw_script_scan 递归扫描，名称 / 说明来自脚本
 * 头部的 -- @name / -- @desc）—— scripts 目录只是官方推荐位置，放别处一样能扫到：
 *   顶部一行：页面标题 + 刷新（重扫整盘）；
 *   正文：脚本列表（图标 + 名称 + 说明，列表自己不带标题）。
 * 点条目直接运行；长按弹出操作单（运行 / 编辑 / 删除），与「文件管理」同一套交互。
 *
 * 本页不显示运行状态、也没有停止按钮：脚本总有自己的一页（脚本界面或「运行日志」页），
 * 离开它脚本就停，所以本页可见时脚本一定没在跑 —— 那两样东西永远是"未运行"、永远隐藏。
 * 停止脚本由脚本页上的返回键 / BOOT 键 / 主页键完成（fw_app_mgr 判断要不要停）。
 *
 * 整盘递归可能要几十到上百毫秒（每个目录一次 opendir），扫描放在一次性任务里做，
 * 扫完再回 LVGL 任务换列表，App 界面不会被它拖住。
 *
 * 脚本界面由脚本自己创建（fw_script 会切屏），在脚本页上按返回键 / 双击 / 主页键 =
 * 结束脚本（fw_app_mgr 里判断，fw_script_stop() 会把屏还给启动脚本的 App）。
 * 脚本出错时错误描述经 fw_script_last_error() 取出，弹对话框说明"为什么没跑起来"。
 *
 * 脚本下载在「下载」App 里做（.lua 会落到 scripts 目录），本页不重复实现。
 */

#include "app_scripts.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.scripts";

#define SCRIPT_SCAN_MIN 32      /* 首次按这么多条分配，扫满了翻倍重扫 */
#define SCAN_TASK_STACK 4096
#define SCAN_TASK_PRIO  3
#define SCAN_TASK_CORE  0

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_list = NULL;

static fw_script_info_t *s_scripts = NULL;
static size_t s_count = 0;
static volatile bool s_scanning = false;    /* 扫描任务在跑（同时只允许一个） */

/* 操作单（长按条目）：整页浮层 + 列表，与「文件管理」同一个做法 */
static lv_obj_t *s_menu = NULL;
static lv_obj_t *s_menu_list = NULL;
static char s_act_path[FW_SCRIPT_PATH_MAX] = { 0 };
static char s_act_name[FW_SCRIPT_NAME_MAX] = { 0 };

/* 脚本启停 / 出错事件：回调跑在事件总线任务里，只置位，界面交给 LVGL 任务 */
static volatile bool s_evt_pending = false;
static char s_shown_error[FW_SCRIPT_ERR_MAX] = { 0 };   /* 已提示过的错误，避免反复弹窗 */

/* 前置声明（条目回调与操作单互相引用） */
static void scan_start(void);
static void menu_open(const char *title);
static void menu_add(const char *text, lv_event_cb_t cb);
static void menu_close(void);
static void item_cb(lv_event_t *e);
static void item_long_cb(lv_event_t *e);
static void act_run_cb(lv_event_t *e);
static void act_edit_cb(lv_event_t *e);
static void act_delete_cb(lv_event_t *e);

/* ---------------------------------- 扫描 ---------------------------------- */

/* 列表没有自己的标题（页面标题在头部行上），刷新时整块清掉重建 */
static void fill_list(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    if (s_count == 0) {
        fw_ui_list_hint(s_list, s_scanning ? "正在扫描…" : "还没有脚本");
        return;
    }

    for (size_t i = 0; i < s_count; i++) {
        /* user_data 传下标 + 1（0 表示无效） */
        const void *idx = (void *)(uintptr_t)(i + 1);
        lv_obj_t *item = fw_ui_list_add_icon(s_list, &icon_ui_file_text, s_scripts[i].name,
                                             (s_scripts[i].desc[0] != '\0') ? s_scripts[i].desc
                                                                            : NULL,
                                             item_cb, (void *)idx);
        if (item == NULL) continue;

        lv_obj_add_event_cb(item, item_long_cb, LV_EVENT_LONG_PRESSED, (void *)idx);
    }
}

/* 扫描任务：整盘递归很花时间，放这里做；扫完回 LVGL 任务里换列表。
 * 结果由 App 接管（发布时整体替换 s_scripts），所以扫描缓冲和界面数组不会互相打架。 */
static void scan_task(void *arg)
{
    (void)arg;

    size_t cap = SCRIPT_SCAN_MIN;
    size_t n = 0;
    fw_script_info_t *out = malloc(sizeof(*out) * cap);

    if (out != NULL) {
        if (fw_script_scan(out, cap, &n) != ESP_OK) {
            n = 0;
        } else {
            /* 扫满了说明还有：翻倍重扫，保证"列全" */
            while (n >= cap && cap < 4096) {
                const size_t cap2 = cap * 2;
                fw_script_info_t *p = realloc(out, sizeof(*p) * cap2);
                if (p == NULL) break;           /* 分配不出来就用现有结果 */
                out = p;
                cap = cap2;

                size_t n2 = 0;
                if (fw_script_scan(out, cap, &n2) != ESP_OK) break;
                n = n2;
            }
        }
    }

    /* 拿 LVGL 锁发布。先把 s_scanning 清掉：发布后列表里的"正在扫描…"提示要能换掉，
     * 也让用户在扫描结束后能再点刷新。这个任务只干这一件事，等锁可以等（20 × 250 ms） */
    s_scanning = false;

    for (int i = 0; i < 20; i++) {
        bool done = false;

        if (lvgl_port_lock(200)) {
            if (s_root != NULL) {
                if (out != NULL) {
                    free(s_scripts);            /* 整体替换：扫描缓冲和界面数组不共用 */
                    s_scripts = out;
                    s_count = n;
                    out = NULL;                 /* 交给 App 了 */
                } else {
                    fw_ui_toast("内存不足，脚本列表未更新", 2200);
                }
                fill_list();
                done = true;
            }
            lvgl_port_unlock();
        }
        if (done) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    free(out);                                  /* App 已销毁或没接管：自己释放 */
    vTaskDelete(NULL);
}

/* 起一次性扫描任务；调用方必须已持 LVGL 锁（只置位 + 建任务） */
static void scan_start(void)
{
    if (s_scanning) return;
    s_scanning = true;

    if (xTaskCreatePinnedToCore(scan_task, "script_scan", SCAN_TASK_STACK, NULL,
                                SCAN_TASK_PRIO, NULL, SCAN_TASK_CORE) != pdPASS) {
        s_scanning = false;
        fw_ui_toast("扫描任务创建失败", 2000);
    }
}

/* ---------------------------------- 操作单 ---------------------------------- */

static void menu_close_cb(lv_event_t *e)
{
    (void)e;
    menu_close();
}

static void menu_open(const char *title)
{
    if (s_menu != NULL || s_root == NULL) return;

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

    /* 第一行：脚本名（撑满）+ 关闭 */
    lv_obj_t *row = lv_obj_create(s_menu);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lb = lv_label_create(row);
    lv_obj_set_flex_grow(lb, 1);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(lb, (title != NULL) ? title : "");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);

    fw_ui_btn(row, "关闭", 52, false, menu_close_cb, NULL);

    s_menu_list = fw_ui_list(s_menu, NULL);
    lv_obj_set_width(s_menu_list, lv_pct(100));
    lv_obj_set_flex_grow(s_menu_list, 1);
}

static void menu_add(const char *text, lv_event_cb_t cb)
{
    if (s_menu_list == NULL) return;
    fw_ui_list_add(s_menu_list, text, cb, NULL);
}

static void menu_close(void)
{
    if (s_menu == NULL) return;

    lv_obj_delete(s_menu);
    s_menu = NULL;
    s_menu_list = NULL;
}

/* ---------------------------------- 动作 ---------------------------------- */

/* 点列表条目 = 运行 */
static void item_cb(lv_event_t *e)
{
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_count || s_scripts == NULL) return;

    if (fw_script_run(s_scripts[idx - 1].path) != ESP_OK) {
        fw_ui_toast("启动失败", 2000);
    }
}

/* 长按条目 = 操作单（运行 / 编辑 / 删除），与「文件管理」一致 */
static void item_long_cb(lv_event_t *e)
{
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_count || s_scripts == NULL) return;

    strlcpy(s_act_path, s_scripts[idx - 1].path, sizeof(s_act_path));
    strlcpy(s_act_name, s_scripts[idx - 1].name, sizeof(s_act_name));

    menu_open(s_act_name);
    /* 脚本实际位置（不一定在 scripts 目录）；路径长，显式设宽让它换行 */
    lv_obj_t *hint = fw_ui_list_hint(s_menu_list, s_act_path);
    if (hint != NULL) {
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    }
    menu_add("运行", act_run_cb);
    menu_add("编辑", act_edit_cb);
    menu_add("删除", act_delete_cb);
}

/* 操作单：运行 s_act_path（已在"当前脚本"里运行的话由 fw_script_run 挡住） */
static void act_run_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    if (s_act_path[0] == '\0') return;

    const esp_err_t err = fw_script_run(s_act_path);
    if (err == ESP_ERR_INVALID_STATE) {
        fw_ui_toast("已有脚本在运行", 2000);
    } else if (err != ESP_OK) {
        fw_ui_toast("启动失败", 2000);
    }
}

/* 操作单：编辑 —— 交给「编辑器」App（它支持 Editor?path=<绝对路径>） */
static void act_edit_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    if (s_act_path[0] == '\0') return;

    char args[FW_SCRIPT_PATH_MAX + 8];      /* "path=" + 路径 + 结尾 */
    const int n = snprintf(args, sizeof(args), "path=%s", s_act_path);
    if (n < 0 || (size_t)n >= sizeof(args) || (size_t)n >= FW_APP_ARGS_MAX) {
        fw_ui_toast("路径太长", 2000);
        return;
    }

    if (fw_app_mgr_launch_with_args("Editor", args) != ESP_OK) {
        fw_ui_toast("打开编辑器失败", 2000);
    }
}

static void del_confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;

    if (btn != FW_DIALOG_BTN_OK || s_act_path[0] == '\0') return;

    const esp_err_t err = svc_storage_remove(s_act_path);
    s_act_path[0] = '\0';

    fw_ui_toast((err == ESP_OK) ? "已删除" : "删除失败", 2000);
    if (err == ESP_OK) {
        scan_start();                   /* 对话框回调在 LVGL 任务里，直接起扫描任务 */
    }
}

static void act_delete_cb(lv_event_t *e)
{
    (void)e;
    menu_close();

    if (s_act_path[0] == '\0') return;

    /* 运行中的脚本不让删：正在执行的 Lua 文件被拔掉会很难收场 */
    if (fw_script_is_running()) {
        fw_ui_toast("请先停止脚本", 2000);
        return;
    }

    fw_ui_dialog(NULL, "删除脚本", s_act_name,
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, del_confirm_cb, NULL);
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;

    if (s_scanning) {
        fw_ui_toast("正在扫描…", 1500);
        return;
    }

    scan_start();
    fw_ui_toast("正在重新扫描…", 1500);
}

/* ---------------------------------- 事件 ---------------------------------- */

/* 出错时弹对话框说明原因（同一段错误只提示一次；没有错误文本就不弹） */
static void show_error_dialog(void)
{
    const char *err = fw_script_last_error();
    if (err == NULL || err[0] == '\0') return;
    if (strcmp(err, s_shown_error) == 0) return;

    strlcpy(s_shown_error, err, sizeof(s_shown_error));

    /* "…/dir/x.lua:12: 描述" → "x.lua:12: 描述"：脚本可能在任何目录，只去掉目录那一段
     * （在第一个冒号之前找最后一个斜杠，别把描述里的斜杠也当成路径分隔符） */
    const char *colon = strchr(err, ':');
    const char *end = (colon != NULL) ? colon : (err + strlen(err));
    const char *start = err;
    for (const char *q = err; q < end; q++) {
        if (*q == '/') start = q + 1;
    }
    err = start;

    fw_ui_dialog(NULL, "脚本出错", err, FW_DIALOG_BTN_OK, NULL, NULL);
}

static void evt_show_error(void *unused)
{
    (void)unused;

    s_evt_pending = false;
    if (s_root == NULL) return;         /* App 已经销毁 */

    /* 只有出错时才需要动界面：说明为什么没跑起来。脚本正常启停不改文件列表，
     * 运行状态也不在这里显示（见文件头注释） */
    show_error_dialog();
}

static void on_script_failed(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    if (s_evt_pending) return;          /* 已经排了一次刷新，事件连着来不用刷两次 */
    s_evt_pending = true;

    /* 事件总线任务的栈要留给订阅回调：拿不到锁就丢掉这次刷新，下一次事件还会刷 */
    if (lvgl_port_lock(100)) {
        lv_async_call(evt_show_error, NULL);
        lvgl_port_unlock();
    } else {
        s_evt_pending = false;
    }
}

/* ---------------------------------- 生命周期 ---------------------------------- */

static void *scripts_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_evt_pending = false;
    s_shown_error[0] = '\0';
    s_menu = NULL;
    s_menu_list = NULL;
    s_act_path[0] = '\0';
    s_act_name[0] = '\0';

    /* 头部一行：页面标题（撑满左侧）+ 刷新（重扫整盘）。脚本没有运行状态可显示，
     * 见文件头注释 */
    lv_obj_t *head = lv_obj_create(body);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_flex_grow(title, 1);         /* 左侧都给标题，刷新按钮贴右边 */
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);   /* 标题只占一行，别把头部挤成两行 */
    lv_label_set_text(title, "脚本管理");
    lv_obj_set_style_text_font(title, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);

    fw_ui_icon_btn(head, &icon_ui_refresh, NULL, 36, refresh_cb, NULL);

    /* 列表：余下的高度都给它（页面标题在上面，列表自己不再带标题）；
     * 扫描在任务里做，先显示"正在扫描…" */
    s_list = fw_ui_list(body, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    fill_list();                        /* s_count 为 0 时显示"正在扫描…" */
    scan_start();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

/* 重新进前台：文件可能被编辑器改过，重扫一次（订阅在 on_start 里做一次就够） */
static void scripts_on_resume(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    scan_start();
    lvgl_port_unlock();
}

static void scripts_on_start(void *ctx)
{
    (void)ctx;

    svc_event_bus_subscribe(SVC_EVENT_SCRIPT_FAILED, on_script_failed, NULL);

    scripts_on_resume(NULL);
}

static void scripts_on_pause(void *ctx)
{
    (void)ctx;

    svc_event_bus_unsubscribe(SVC_EVENT_SCRIPT_FAILED, on_script_failed);
}

static void scripts_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_menu = NULL;                  /* 随根屏一起删掉，避免悬空 */
    s_menu_list = NULL;
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_list = NULL;
    free(s_scripts);
    s_scripts = NULL;
    s_count = 0;
    s_scanning = false;             /* 在跑的扫描任务扫到一半会自己发现 App 没了 */
    lvgl_port_unlock();

    s_evt_pending = false;
    s_act_path[0] = '\0';
    s_act_name[0] = '\0';
}

/* 操作单开着时，返回键先关操作单（脚本页上的返回由 fw_app_mgr 处理） */
static bool scripts_on_back(void *ctx)
{
    (void)ctx;

    if (s_menu == NULL) return false;

    menu_close();
    return true;
}

const fw_app_desc_t app_scripts_desc = {
    .name = "Scripts",
    .title = "脚本管理",
    .icon = &icon_home_scripts,
    .on_create = scripts_on_create,
    .on_start = scripts_on_start,
    .on_pause = scripts_on_pause,
    .on_resume = scripts_on_resume,
    .on_destroy = scripts_on_destroy,
    .on_back = scripts_on_back,
};
