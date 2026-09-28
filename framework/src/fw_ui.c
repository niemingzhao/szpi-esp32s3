/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - UI 通用组件实现
 */

#include "fw_ui.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "fw.ui";

#define FW_DIALOG_BTN_MAX   4

typedef struct {
    lv_obj_t *scrim;
    fw_dialog_cb_t cb;
    void *user;
} fw_dialog_state_t;

typedef struct {
    fw_dialog_btn_t btn;
} fw_dialog_btn_ctx_t;

static fw_dialog_state_t s_dialog;
static fw_dialog_btn_ctx_t s_dialog_btns[FW_DIALOG_BTN_MAX];

/* 按钮排列顺序（决定对话框里从左到右的次序） */
static const fw_dialog_btn_t k_btn_order[] = {
    FW_DIALOG_BTN_YES, FW_DIALOG_BTN_NO, FW_DIALOG_BTN_OK,
    FW_DIALOG_BTN_SHUTDOWN, FW_DIALOG_BTN_REBOOT, FW_DIALOG_BTN_CANCEL,
};

static const char *btn_text(fw_dialog_btn_t b)
{
    switch (b) {
    case FW_DIALOG_BTN_OK:       return "确定";
    case FW_DIALOG_BTN_CANCEL:   return "取消";
    case FW_DIALOG_BTN_YES:      return "是";
    case FW_DIALOG_BTN_NO:       return "否";
    case FW_DIALOG_BTN_SHUTDOWN: return "关机";
    case FW_DIALOG_BTN_REBOOT:   return "重启";
    default:                     return "";
    }
}

esp_err_t fw_ui_init(void)
{
    memset(&s_dialog, 0, sizeof(s_dialog));
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

/* ---------------------------------- 进度条 ---------------------------------- */

lv_obj_t *fw_ui_progress_bar(lv_obj_t *parent, const char *title)
{
    lvgl_port_lock(0);

    lv_obj_t *box = lv_obj_create(parent != NULL ? parent : lv_layer_top());
    lv_obj_set_size(box, 240, 72);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(box, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 12, 0);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, title != NULL ? title : "");
    lv_obj_set_style_text_font(label, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *bar = lv_bar_create(box);
    lv_obj_set_size(bar, 216, 14);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, fw_theme_color_divider(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, fw_theme_color_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lvgl_port_unlock();
    return box;
}

esp_err_t fw_ui_progress_set(lv_obj_t *bar, uint8_t percent)
{
    if (bar == NULL) return ESP_ERR_INVALID_ARG;
    if (percent > 100) percent = 100;

    lvgl_port_lock(0);
    /* 子对象顺序固定：0 = 标题 label，1 = lv_bar */
    lv_obj_t *inner = lv_obj_get_child(bar, 1);
    if (inner != NULL) {
        lv_bar_set_value(inner, (int32_t)percent, LV_ANIM_ON);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

/* --------------------------------- 对话框 --------------------------------- */

static void dialog_btn_cb(lv_event_t *e)
{
    fw_dialog_btn_ctx_t *ctx = (fw_dialog_btn_ctx_t *)lv_event_get_user_data(e);
    if (ctx == NULL) return;

    fw_dialog_btn_t btn = ctx->btn;
    fw_dialog_cb_t cb = s_dialog.cb;
    void *user = s_dialog.user;

    fw_ui_dialog_close(NULL);
    if (cb != NULL) cb(btn, user);
}

lv_obj_t *fw_ui_dialog(lv_obj_t *parent, const char *title, const char *msg,
                       fw_dialog_btn_t buttons, fw_dialog_cb_t cb, void *user)
{
    if (buttons == FW_DIALOG_BTN_NONE) return NULL;
    if (s_dialog.scrim != NULL) {
        ESP_LOGW(TAG, "another dialog is already open");
        return NULL;
    }

    lvgl_port_lock(0);

    lv_obj_t *scrim = lv_obj_create(parent != NULL ? parent : lv_layer_top());
    lv_obj_set_size(scrim, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_set_size(panel, 264, 150);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);

    if (title != NULL) {
        lv_obj_t *l = lv_label_create(panel);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_font(l, fw_asset_font_24(), 0);
        lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    if (msg != NULL) {
        lv_obj_t *m = lv_label_create(panel);
        lv_label_set_text(m, msg);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, 240);
        lv_obj_set_style_text_font(m, fw_asset_font_20(), 0);
        lv_obj_set_style_text_color(m, fw_theme_color_text_secondary(), 0);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 32);
    }

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, 240, 40);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    uint8_t n = 0;
    for (size_t i = 0; i < sizeof(k_btn_order) / sizeof(k_btn_order[0]); i++) {
        if ((buttons & k_btn_order[i]) == 0) continue;
        if (n >= FW_DIALOG_BTN_MAX) break;

        s_dialog_btns[n].btn = k_btn_order[i];

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 84, 34);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, dialog_btn_cb, LV_EVENT_CLICKED, &s_dialog_btns[n]);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, btn_text(k_btn_order[i]));
        lv_obj_center(bl);
        n++;
    }

    s_dialog.scrim = scrim;
    s_dialog.cb = cb;
    s_dialog.user = user;

    lvgl_port_unlock();
    return scrim;
}

esp_err_t fw_ui_dialog_close(lv_obj_t *dlg)
{
    lvgl_port_lock(0);
    if (s_dialog.scrim != NULL && (dlg == NULL || dlg == s_dialog.scrim)) {
        lv_obj_del_async(s_dialog.scrim);
        s_dialog.scrim = NULL;
        s_dialog.cb = NULL;
        s_dialog.user = NULL;
    }
    lvgl_port_unlock();
    return ESP_OK;
}

/* ---------------------------------- Toast --------------------------------- */

static void toast_timer_cb(lv_timer_t *t)
{
    lv_obj_t *obj = (lv_obj_t *)t->user_data;
    if (obj != NULL) lv_obj_del_async(obj);
}

lv_obj_t *fw_ui_toast(const char *msg, uint32_t duration_ms)
{
    if (msg == NULL) return NULL;
    if (duration_ms == 0) duration_ms = 3000;

    lvgl_port_lock(0);

    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, 264, LV_SIZE_CONTENT);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(toast, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_pad_all(toast, 10, 0);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, msg);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 240);
    lv_obj_set_style_text_font(label, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_center(label);

    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -44);

    lv_timer_t *timer = lv_timer_create(toast_timer_cb, duration_ms, toast);
    lv_timer_set_repeat_count(timer, 1);

    lvgl_port_unlock();
    return toast;
}

/* ---------------------------------- 列表 ---------------------------------- */

lv_obj_t *fw_ui_list(lv_obj_t *parent, const char *title)
{
    lvgl_port_lock(0);

    lv_obj_t *box = lv_obj_create(parent != NULL ? parent : lv_layer_top());
    lv_obj_set_style_bg_color(box, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);

    if (title != NULL) {
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_font(l, fw_asset_font_24(), 0);
        lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    }

    lvgl_port_unlock();
    return box;
}

lv_obj_t *fw_ui_list_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *user)
{
    if (list == NULL) return NULL;

    lvgl_port_lock(0);

    lv_obj_t *btn = lv_btn_create(list);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 36);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text != NULL ? text : "");
    lv_obj_set_style_text_font(l, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);

    lvgl_port_unlock();
    return btn;
}

/* ---------------------------------- 网格 ---------------------------------- */

lv_obj_t *fw_ui_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t item_w, lv_coord_t item_h)
{
    (void)item_h;
    if (cols == 0) cols = 1;

    lvgl_port_lock(0);

    lv_obj_t *g = lv_obj_create(parent != NULL ? parent : lv_layer_top());
    lv_obj_set_size(g, (lv_coord_t)(cols * item_w + (cols - 1) * 8 + 12), LV_SIZE_CONTENT);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 6, 0);
    lv_obj_set_style_pad_column(g, 8, 0);
    lv_obj_set_style_pad_row(g, 10, 0);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lvgl_port_unlock();
    return g;
}
