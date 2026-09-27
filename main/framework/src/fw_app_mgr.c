/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - App Manager 实现
 *
 * 返回栈模型：栈底为 Home。launch() 把目标 App 压栈并暂停原前台 App；
 * back() 弹栈并恢复上一个 App。App 在后台保留其 LVGL 资源（on_pause / on_resume）。
 *
 * 换主题时只重建前台 App，其余已创建的 App 标为 stale（控件配色是写死在控件上的，
 * 不重建就还是旧配色），等下次显示前再按新主题重建 —— 否则 App 越多切换越慢，
 * 天气这类 App 还会白拉一次网络请求。
 */

#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "fw.app_mgr";

#define FW_APP_MAX        24
#define FW_APP_STACK_MAX  8
#define FW_APP_ANIM_MS    250

typedef struct {
    const fw_app_desc_t *desc;
    void *ctx;
    lv_obj_t *root;
    bool created;
    bool active;
    bool stale;      /* 换主题后界面还是旧配色，下次显示之前先重建 */
} fw_app_slot_t;

static fw_app_slot_t s_slots[FW_APP_MAX];
static size_t s_count = 0;
static int s_stack[FW_APP_STACK_MAX];
static int s_top = -1;
static char s_launch_args[FW_APP_ARGS_MAX];

static int app_find(const char *name)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_slots[i].desc != NULL && strcmp(s_slots[i].desc->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int stack_find(int app_idx)
{
    for (int i = 0; i <= s_top; i++) {
        if (s_stack[i] == app_idx) return i;
    }
    return -1;
}

static void stack_remove(int pos)
{
    for (int i = pos; i < s_top; i++) {
        s_stack[i] = s_stack[i + 1];
    }
    s_top--;
}

static void slot_pause(int app_idx)
{
    fw_app_slot_t *a = &s_slots[app_idx];
    if (!a->active) return;

    a->active = false;
    if (a->desc->on_pause) a->desc->on_pause(a->ctx);
}

/* 保证 App 的界面已按当前主题创建好：stale 的先按 on_destroy 删掉旧界面再重建。
 * 返回 true 表示这次做了创建 / 重建，调用方要按"首次进入"发 on_start */
static bool slot_ensure_created(fw_app_slot_t *a)
{
    if (a->created && !a->stale) return false;

    if (a->stale) {
        a->stale = false;
        if (a->created && a->desc->on_destroy != NULL) a->desc->on_destroy(a->ctx);
        a->created = false;
        a->ctx = NULL;
        a->root = NULL;
    }

    a->root = (lv_obj_t *)a->desc->on_create();
    a->created = (a->root != NULL);
    a->ctx = a->root;
    return true;
}

esp_err_t fw_app_mgr_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_stack, 0, sizeof(s_stack));
    s_count = 0;
    s_top = -1;

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t fw_app_mgr_register(const fw_app_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL || desc->on_create == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_find(desc->name) >= 0) {
        ESP_LOGW(TAG, "duplicate app: %s", desc->name);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= FW_APP_MAX) {
        return ESP_ERR_NO_MEM;
    }

    s_slots[s_count].desc = desc;
    s_count++;

    ESP_LOGI(TAG, "registered app: %s", desc->name);
    return ESP_OK;
}

static esp_err_t app_launch(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;

    int idx = app_find(name);
    if (idx < 0) {
        ESP_LOGW(TAG, "app not found: %s", name);
        return ESP_ERR_NOT_FOUND;
    }

    lvgl_port_lock(0);

    if (s_top >= 0 && s_stack[s_top] == idx) {
        lvgl_port_unlock();
        return ESP_OK;
    }

    /* 栈满要在动任何状态之前检查：否则会把目标 App 创建出来却压不进栈 */
    if (s_top + 1 >= FW_APP_STACK_MAX) {
        lvgl_port_unlock();
        ESP_LOGW(TAG, "app stack full (%d)", FW_APP_STACK_MAX);
        return ESP_ERR_INVALID_STATE;
    }

    const int prev = (s_top >= 0) ? s_stack[s_top] : -1;

    if (s_top >= 0) {
        slot_pause(s_stack[s_top]);
    }

    int pos = stack_find(idx);
    if (pos >= 0) {
        stack_remove(pos);
    }

    fw_app_slot_t *a = &s_slots[idx];
    const bool fresh = slot_ensure_created(a);   /* 首次创建，或换主题后的重建 */
    if (!a->created) {
        /* 目标 App 没建起来：把刚暂停的原前台 App 恢复回去，
         * 否则它会停在"界面还显示着、定时器已经停了"的状态 */
        if (prev >= 0) {
            fw_app_slot_t *p = &s_slots[prev];
            if (!p->active) {
                p->active = true;
                if (p->desc->on_resume) p->desc->on_resume(p->ctx);
            }
        }
        lvgl_port_unlock();
        ESP_LOGE(TAG, "on_create failed: %s", name);
        return ESP_FAIL;
    }

    s_stack[++s_top] = idx;
    a->active = true;

    if (pos >= 0 && !fresh) {
        /* 从返回栈里回来的：只发 on_resume */
        if (a->desc->on_resume) a->desc->on_resume(a->ctx);
    } else {
        if (a->desc->on_start) a->desc->on_start(a->ctx);
        /* 之前创建过、只是不在返回栈里（用主页键回桌面后再点进来，或 App 之间跳转）：
         * on_resume 也要给 —— 在 on_pause 里停掉刷新定时器的 App 只靠 on_start 不会
         * 恢复，界面看着正常但不再刷新（时钟停跳就是这个原因）。 */
        if (!fresh && a->desc->on_resume) a->desc->on_resume(a->ctx);
    }

    fw_window_switch_to(a->root, LV_SCREEN_LOAD_ANIM_FADE_IN, FW_APP_ANIM_MS);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "launched app: %s", name);
    return ESP_OK;
}

esp_err_t fw_app_mgr_launch(const char *name)
{
    s_launch_args[0] = '\0';
    return app_launch(name);
}

esp_err_t fw_app_mgr_launch_with_args(const char *name, const char *args)
{
    if (args != NULL) {
        strncpy(s_launch_args, args, sizeof(s_launch_args) - 1);
        s_launch_args[sizeof(s_launch_args) - 1] = '\0';
    } else {
        s_launch_args[0] = '\0';
    }
    return app_launch(name);
}

esp_err_t fw_app_mgr_launch_uri(const char *uri)
{
    if (uri == NULL) return ESP_ERR_INVALID_ARG;

    /* 形如 "szpi://Music?song=1" 或 "Music?song=1" */
    const char *p = strstr(uri, "://");
    p = (p != NULL) ? (p + 3) : uri;

    const char *q = strchr(p, '?');
    size_t n = (q != NULL) ? (size_t)(q - p) : strlen(p);
    if (n == 0 || n >= FW_APP_ARGS_MAX) return ESP_ERR_INVALID_ARG;

    char name[FW_APP_ARGS_MAX];
    memcpy(name, p, n);
    name[n] = '\0';

    return fw_app_mgr_launch_with_args(name, (q != NULL) ? (q + 1) : NULL);
}

const char *fw_app_mgr_get_args(void)
{
    return s_launch_args;
}

esp_err_t fw_app_mgr_back(void)
{
    /* 脚本页在前台时，返回键 = 结束脚本：fw_script 停止时会把屏还给启动脚本的那个 App
     *（正常就是脚本管理页），和「音乐 / 秒表 / 计时器」的"退出即停"手感一致。
     * 不这么做的话，脚本界面会一直悬在后台、再也回不去，脚本还在跑。 */
    if (fw_script_owns_screen()) {
        fw_script_stop();
        return ESP_OK;
    }

    lvgl_port_lock(0);

    if (s_top <= 0) {
        lvgl_port_unlock();
        return ESP_OK;
    }

    /* 先让当前 App 自己处理（如返回上一级页面） */
    fw_app_slot_t *cur = &s_slots[s_stack[s_top]];
    if (cur->desc->on_back != NULL && cur->desc->on_back(cur->ctx)) {
        lvgl_port_unlock();
        return ESP_OK;
    }

    slot_pause(s_stack[s_top]);
    s_top--;

    int idx = s_stack[s_top];
    fw_app_slot_t *a = &s_slots[idx];

    /* 换主题后还没重建的 App：现在要显示了，先按新主题重建一次 */
    const bool fresh = slot_ensure_created(a);
    a->active = true;
    if (fresh && a->desc->on_start) a->desc->on_start(a->ctx);
    if (a->desc->on_resume) a->desc->on_resume(a->ctx);

    if (a->root != NULL) {
        fw_window_switch_to(a->root, LV_SCREEN_LOAD_ANIM_FADE_IN, FW_APP_ANIM_MS);
    } else {
        ESP_LOGE(TAG, "rebuild failed on back: %s", a->desc->name);
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "back to: %s", a->desc->name);
    return ESP_OK;
}

esp_err_t fw_app_mgr_back_to_home(void)
{
    /* 脚本页在前台时先结束脚本（同 fw_app_mgr_back()）：这里不用把屏还给脚本页，
     * 脚本任务收尾时发现前台已经不是它，就只删掉自己的页 */
    const bool from_script = fw_script_owns_screen();
    if (from_script) {
        fw_script_stop();
    }

    int home = app_find(FW_APP_HOME_NAME);
    if (home < 0) return ESP_ERR_NOT_FOUND;

    lvgl_port_lock(0);

    /* 栈底已经是桌面、脚本页也没在前台：真的什么都不用做。
     * 脚本页在前台时不能只看栈就提前返回：脚本任务可能抢在我们前面把屏切回脚本管理页
     * （它看到脚本页还在前台就切回去了），而这里已经/即将把栈压到桌面 —— 结果就是
     * 栈说"在桌面"、屏上却是脚本管理页，之后按返回也会因为栈底弹不动而回不来。
     * 真回到桌面这一条路径必须保证"桌面真的被切到前台"。 */
    if (!from_script && s_top == 0 && s_stack[0] == home) {
        lvgl_port_unlock();
        return ESP_OK;
    }

    if (s_top >= 0) {
        slot_pause(s_stack[s_top]);
    }

    fw_app_slot_t *h = &s_slots[home];

    /* Home 也可能在换主题时被标成待重建（当时前台是别的 App） */
    const bool fresh = slot_ensure_created(h);
    if (!h->created) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_top = 0;
    s_stack[0] = home;
    h->active = true;
    if (fresh && h->desc->on_start) h->desc->on_start(h->ctx);
    if (h->desc->on_resume) h->desc->on_resume(h->ctx);
    /* 从脚本页离开这一下必须直接切（NONE + 0 ms）：带动画的切屏会让 LVGL 把脚本页
     * 记在 display 的 prev_scr 上，而脚本任务紧接着就把那一页删掉 —— 下一帧刷新走到
     * prev_scr 就是一块已释放的内存（实测崩在 lv_display_refr_timer →
     * lv_obj_update_layout(prev_scr)）。直接切时 LVGL 只把 prev_scr 清空、不记旧屏。 */
    fw_window_switch_to(h->root,
                        from_script ? LV_SCREEN_LOAD_ANIM_NONE : LV_SCREEN_LOAD_ANIM_FADE_IN,
                        from_script ? 0 : FW_APP_ANIM_MS);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "back to: %s", FW_APP_HOME_NAME);
    return ESP_OK;
}

/* 换主题：立刻重建前台 App；其余已创建的 App 只标"待重建"，等它下次显示前再重建
 * （见文件头：全量重建随 App 数量线性变慢，天气这类 App 还会白拉一次网络请求） */
esp_err_t fw_app_mgr_rebuild_all(void)
{
    lvgl_port_lock(0);

    /* 换主题会把屏切到临时空屏再切回来：脚本页正显示时按"离开即停"处理（同 back /
     * back_to_home 的判断）。不这么做的话，重建会把前台换成脚本管理页，而脚本还在后台
     * 跑、它的页面再也回不去 */
    if (fw_script_owns_screen()) {
        fw_script_stop();
    }

    /* 先把前台切到一块临时空屏：否则删除"正在显示的屏"会让 LVGL 的 act_scr 变成 NULL */
    lv_obj_t *dummy = lv_obj_create(NULL);
    lv_obj_set_scrollable(dummy, false);
    lv_obj_set_style_bg_color(dummy, fw_theme_color_bg_primary(), 0);
    lv_screen_load(dummy);
    fw_window_sync_active();   /* 同步内部记录，避免旧屏地址被新屏复用时误判 */

    const int fg = (s_top >= 0) ? s_stack[s_top] : -1;

    /* 后台 App：只标记待重建（不销毁，后台 App 的定时器 / 状态不受影响），
     * 下次被显示时由 slot_ensure_created() 按新主题重建 */
    for (size_t i = 0; i < s_count; i++) {
        fw_app_slot_t *a = &s_slots[i];
        if (!a->created || (int)i == fg) continue;
        a->stale = true;
    }

    /* 前台 App 立刻重建（走和"待重建"同一条路径）：先 on_pause（退订 / 停刷新）
     * 再 on_start / on_resume 恢复前台状态。on_start 与 on_resume 都要给：用 on_start
     * 订阅的 App（Wi-Fi、蓝牙、日志等）靠 on_pause 退订，用 on_pause / on_resume 的
     * App（时钟、音乐、姿态仪、相机、录音机等）靠 on_pause 暂停刷新定时器 —— 只调
     * on_start 会让后者的定时器一直停着，界面看着"活着"其实不再刷新。 */
    lv_obj_t *target = NULL;
    if (fg >= 0) {
        fw_app_slot_t *a = &s_slots[fg];
        a->stale = a->created;
        slot_ensure_created(a);
        if (a->created) {
            if (a->desc->on_pause != NULL) a->desc->on_pause(a->ctx);
            if (a->desc->on_start != NULL) a->desc->on_start(a->ctx);
            if (a->desc->on_resume != NULL) a->desc->on_resume(a->ctx);
            target = a->root;
        }
    }

    /* 当前 App 重建失败时退到第一个重建成功的 App，别把临时屏留在前台 */
    if (target == NULL) {
        for (size_t i = 0; i < s_count; i++) {
            if (s_slots[i].created && s_slots[i].root != NULL) {
                ESP_LOGW(TAG, "current app missing after rebuild, fallback to %s",
                         s_slots[i].desc->name);
                target = s_slots[i].root;
                break;
            }
        }
    }

    if (target == NULL) {
        ESP_LOGW(TAG, "no app could be rebuilt, keep the temporary screen");
    }

    if (target != NULL) {
        fw_window_switch_to(target, LV_SCREEN_LOAD_ANIM_NONE, 0);
    }

    /* 只有它不再是前台屏时才删除（禁止删除活动屏） */
    if (lv_screen_active() != dummy) lv_obj_delete(dummy);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "rebuilt current app for theme change (others marked stale)");
    return ESP_OK;
}

size_t fw_app_mgr_list(const fw_app_desc_t **out, size_t max){
    if (out == NULL) return 0;

    size_t n = (s_count < max) ? s_count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_slots[i].desc;
    }
    return n;
}

size_t fw_app_mgr_count(void)
{
    return s_count;
}

bool fw_app_mgr_is_foreground(const char *name)
{
    if (name == NULL) return false;

    const int i = app_find(name);
    if (i < 0 || !s_slots[i].created) return false;

    /* slot_pause() 是先清 active 再回调 on_pause（= 真的离开），
     * rebuild_all() 是原地重建、不动 active（= 还当前台） */
    return s_slots[i].active;
}
