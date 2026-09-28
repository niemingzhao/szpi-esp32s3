/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - App Manager 实现
 *
 * 返回栈模型：栈底为 Home。launch() 把目标 App 压栈并暂停原前台 App；
 * back() 弹栈并恢复上一个 App。App 在后台保留其 LVGL 资源（on_pause /
 * on_resume），只有 fw_app_mgr_close() 才真正 on_destroy。
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
} fw_app_slot_t;

static fw_app_slot_t s_slots[FW_APP_MAX];
static size_t s_count = 0;
static int s_stack[FW_APP_STACK_MAX];
static int s_top = -1;

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

esp_err_t fw_app_mgr_launch(const char *name)
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

    if (s_top >= 0) {
        slot_pause(s_stack[s_top]);
    }

    int pos = stack_find(idx);
    if (pos >= 0) {
        stack_remove(pos);
    }

    fw_app_slot_t *a = &s_slots[idx];
    if (!a->created) {
        a->root = (lv_obj_t *)a->desc->on_create();
        if (a->root == NULL) {
            lvgl_port_unlock();
            ESP_LOGE(TAG, "on_create failed: %s", name);
            return ESP_FAIL;
        }
        a->ctx = a->root;
        a->created = true;
    }

    if (s_top + 1 >= FW_APP_STACK_MAX) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_stack[++s_top] = idx;
    a->active = true;

    if (pos >= 0) {
        if (a->desc->on_resume) a->desc->on_resume(a->ctx);
    } else {
        if (a->desc->on_start) a->desc->on_start(a->ctx);
    }

    fw_window_switch_to(a->root, LV_SCR_LOAD_ANIM_FADE_IN, FW_APP_ANIM_MS);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "launched app: %s", name);
    return ESP_OK;
}

esp_err_t fw_app_mgr_back(void)
{
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
    a->active = true;
    if (a->desc->on_resume) a->desc->on_resume(a->ctx);
    fw_window_switch_to(a->root, LV_SCR_LOAD_ANIM_FADE_IN, FW_APP_ANIM_MS);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "back to: %s", a->desc->name);
    return ESP_OK;
}

esp_err_t fw_app_mgr_back_to_home(void)
{
    int home = app_find(FW_APP_HOME_NAME);
    if (home < 0) return ESP_ERR_NOT_FOUND;

    lvgl_port_lock(0);

    if (s_top == 0 && s_stack[0] == home) {
        lvgl_port_unlock();
        return ESP_OK;
    }

    if (s_top >= 0) {
        slot_pause(s_stack[s_top]);
    }

    fw_app_slot_t *h = &s_slots[home];
    if (!h->created) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_top = 0;
    s_stack[0] = home;
    h->active = true;
    if (h->desc->on_resume) h->desc->on_resume(h->ctx);
    fw_window_switch_to(h->root, LV_SCR_LOAD_ANIM_FADE_IN, FW_APP_ANIM_MS);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "back to home");
    return ESP_OK;
}

esp_err_t fw_app_mgr_close(const char *name)
{
    int idx = app_find(name);
    if (idx < 0) return ESP_ERR_NOT_FOUND;

    lvgl_port_lock(0);

    fw_app_slot_t *a = &s_slots[idx];
    if (a->active) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;   /* 前台 App 请先 back() */
    }

    int pos = stack_find(idx);
    if (pos >= 0) {
        stack_remove(pos);
    }

    if (a->created) {
        if (a->desc->on_destroy) a->desc->on_destroy(a->ctx);
        a->created = false;
        a->ctx = NULL;
        a->root = NULL;
        a->active = false;
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "closed app: %s", name);
    return ESP_OK;
}

const char *fw_app_mgr_current(void)
{
    if (s_top < 0) return NULL;
    return s_slots[s_stack[s_top]].desc->name;
}

/* 换主题：重建所有已创建 App 的界面，并保持当前前台 App 与它的生命周期 */
esp_err_t fw_app_mgr_rebuild_all(void)
{
    lvgl_port_lock(0);

    bool was_created[FW_APP_MAX];
    for (size_t i = 0; i < FW_APP_MAX; i++) was_created[i] = s_slots[i].created;

    /* 先把前台切到一块临时空屏：否则删除"正在显示的屏"会让 LVGL 的 act_scr 变成 NULL */
    lv_obj_t *dummy = lv_obj_create(NULL);
    lv_obj_clear_flag(dummy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dummy, fw_theme_color_bg_primary(), 0);
    lv_scr_load(dummy);
    fw_window_sync_active();   /* 同步内部记录，避免旧屏地址被新屏复用时误判 */

    for (size_t i = 0; i < s_count; i++) {
        fw_app_slot_t *a = &s_slots[i];
        if (!was_created[i]) continue;

        if (a->desc->on_destroy != NULL) a->desc->on_destroy(a->ctx);
        a->created = false;
        a->ctx = NULL;
        a->root = NULL;
    }

    for (size_t i = 0; i < s_count; i++) {
        fw_app_slot_t *a = &s_slots[i];
        if (!was_created[i]) continue;

        a->root = (lv_obj_t *)a->desc->on_create();
        a->created = (a->root != NULL);
        a->ctx = a->root;
    }

    /* 重建当前前台 App：先 on_pause 退订，避免 on_start 里重复订阅 */
    if (s_top >= 0) {
        fw_app_slot_t *a = &s_slots[s_stack[s_top]];
        if (a->created) {
            if (a->desc->on_pause != NULL) a->desc->on_pause(a->ctx);
            if (a->desc->on_start != NULL) a->desc->on_start(a->ctx);
            if (a->root != NULL) fw_window_switch_to(a->root, LV_SCR_LOAD_ANIM_NONE, 0);
        }
    }

    /* 只有它不再是前台屏时才删除（禁止删除活动屏） */
    if (lv_scr_act() != dummy) lv_obj_del(dummy);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "rebuilt apps for theme change");
    return ESP_OK;
}

bool fw_app_mgr_is_home(void)
{
    int home = app_find(FW_APP_HOME_NAME);
    return (s_top >= 0 && home >= 0 && s_stack[s_top] == home);
}

size_t fw_app_mgr_list(const fw_app_desc_t **out, size_t max)
{
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
