/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Notification Center 实现
 *
 * 由状态栏的“通知”按钮打开，占满状态栏以下的显示区，右上角 × 关闭。
 * 订阅 SVC_EVENT_NOTIFICATION_POSTED / DISMISSED 刷新列表，POSTED 时弹 Toast。
 */

#include "fw_notification.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "fw.notification";

static lv_obj_t *s_scrim = NULL;
static lv_obj_t *s_list = NULL;
static bool s_visible = false;

static void noti_item_cb(lv_event_t *e)
{
    /* 用通知 ID 而不是列表下标：列表重建后同帧点击也不会取错条目 */
    uint32_t noti_id = (uint32_t)(intptr_t)lv_event_get_user_data(e);

    svc_notification_t noti;
    if (svc_notification_find(noti_id, &noti) == ESP_OK && noti.on_click != NULL) {
        noti.on_click(noti.id, noti.user_data);
    }
    fw_notification_hide();
}

static void list_rebuild(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    size_t n = svc_notification_get_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "暂无通知");
        lv_obj_set_style_text_font(empty, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(empty, fw_theme_color_text_disabled(), 0);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        svc_notification_t noti;
        if (svc_notification_get(i, &noti) != ESP_OK) continue;

        char text[192];
        snprintf(text, sizeof(text), "%s: %s",
                 noti.title != NULL ? noti.title : "",
                 noti.message != NULL ? noti.message : "");

        fw_ui_list_add(s_list, text, noti_item_cb, (void *)(intptr_t)noti.id);
    }
}

static void scrim_cb(lv_event_t *e)
{
    (void)e;
    fw_notification_hide();
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    svc_notification_clear_all();
}

static void evt_posted(const svc_event_t *evt, void *user)
{
    (void)user;

    const svc_notification_t *n = (const svc_notification_t *)evt->data;
    if (n != NULL) {
        char msg[160];
        if (n->title != NULL && n->message != NULL) {
            snprintf(msg, sizeof(msg), "%s: %s", n->title, n->message);
        } else if (n->title != NULL) {
            snprintf(msg, sizeof(msg), "%s", n->title);
        } else if (n->message != NULL) {
            snprintf(msg, sizeof(msg), "%s", n->message);
        } else {
            msg[0] = '\0';
        }
        if (msg[0] != '\0') {
            fw_ui_toast(msg, 3000);
        }
    }

    if (s_visible) {
        lvgl_port_lock(0);
        list_rebuild();
        lvgl_port_unlock();
    }
}

static void evt_dismissed(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    if (s_visible) {
        lvgl_port_lock(0);
        list_rebuild();
        lvgl_port_unlock();
    }
}

static void panel_build(lv_coord_t w, lv_coord_t h)
{
    /* 遮罩只覆盖状态栏以下，状态栏按钮始终可用 */
    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scrim, w, h);
    lv_obj_set_pos(s_scrim, 0, FW_STATUSBAR_H);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_scrim, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);

    lv_obj_t *panel = lv_obj_create(s_scrim);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* 标题栏 */
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "通知");
    lv_obj_set_style_text_font(title, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 14);

    lv_obj_t *close = lv_btn_create(panel);
    lv_obj_set_size(close, 38, 28);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_set_style_bg_color(close, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(close, 6, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_border_color(close, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(close, scrim_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(close);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(xl, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(xl, fw_theme_color_text_primary(), 0);
    lv_obj_center(xl);

    lv_obj_t *clear = lv_btn_create(panel);
    lv_obj_set_size(clear, 68, 28);
    lv_obj_align(clear, LV_ALIGN_TOP_RIGHT, -58, 10);
    lv_obj_set_style_bg_color(clear, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(clear, 6, 0);
    lv_obj_set_style_shadow_width(clear, 0, 0);
    lv_obj_set_style_border_width(clear, 1, 0);
    lv_obj_set_style_border_color(clear, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(clear, clear_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clear);
    lv_label_set_text(cl, "清空");
    lv_obj_set_style_text_font(cl, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(cl, fw_theme_color_text_primary(), 0);
    lv_obj_center(cl);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_set_size(line, w - 24, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(line, fw_theme_color_divider(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    s_list = fw_ui_list(panel, NULL);
    lv_obj_set_size(s_list, w - 16, h - 56);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 50);
}

esp_err_t fw_notification_init(void)
{
    lvgl_port_lock(0);
    panel_build(lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL) - FW_STATUSBAR_H);
    list_rebuild();
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    lvgl_port_unlock();

    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_POSTED, evt_posted, NULL);
    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_DISMISSED, evt_dismissed, NULL);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

/* 换主题时重建全部控件 */
esp_err_t fw_notification_rebuild(void)
{
    lvgl_port_lock(0);

    if (s_scrim != NULL) {
        lv_obj_del(s_scrim);
        s_scrim = NULL;
        s_list = NULL;
    }

    panel_build(lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL) - FW_STATUSBAR_H);
    list_rebuild();
    if (s_visible) lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_notification_show(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    list_rebuild();
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_notification_hide(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_notification_toggle(void)
{
    return s_visible ? fw_notification_hide() : fw_notification_show();
}

esp_err_t fw_notification_clear_all(void)
{
    return svc_notification_clear_all();
}

bool fw_notification_is_visible(void)
{
    return s_visible;
}
