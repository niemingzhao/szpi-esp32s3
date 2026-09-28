/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Notification Center 实现
 *
 * 由 fw_input 在“下滑”手势时打开；点击遮罩关闭。
 * 订阅 SVC_EVENT_NOTIFICATION_POSTED / DISMISSED 刷新列表，POSTED 时弹 Toast。
 */

#include "fw_notification.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "fw.notification";

#define NC_PANEL_TOP   24
#define NC_PANEL_H     168

static lv_obj_t *s_scrim = NULL;
static lv_obj_t *s_list = NULL;
static bool s_visible = false;

static void noti_item_cb(lv_event_t *e)
{
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);

    svc_notification_t noti;
    if (svc_notification_get(idx, &noti) == ESP_OK && noti.on_click != NULL) {
        noti.on_click(noti.id, noti.user_data);
    }
    fw_notification_hide();
}

static void rebuild(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    size_t n = svc_notification_get_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "暂无通知");
        lv_obj_set_style_text_font(empty, fw_asset_font_20(), 0);
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

        fw_ui_list_add(s_list, text, noti_item_cb, (void *)(intptr_t)i);
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
        rebuild();
        lvgl_port_unlock();
    }
}

static void evt_dismissed(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    if (s_visible) {
        lvgl_port_lock(0);
        rebuild();
        lvgl_port_unlock();
    }
}

esp_err_t fw_notification_init(void)
{
    lvgl_port_lock(0);

    s_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scrim, lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL));
    lv_obj_set_pos(s_scrim, 0, 0);
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scrim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_scrim, 0, 0);
    lv_obj_set_style_radius(s_scrim, 0, 0);
    lv_obj_set_style_pad_all(s_scrim, 0, 0);
    lv_obj_add_event_cb(s_scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_scrim);
    lv_obj_set_size(panel, lv_disp_get_hor_res(NULL), NC_PANEL_H);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, NC_PANEL_TOP);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "通知");
    lv_obj_set_style_text_font(title, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *clear = lv_btn_create(panel);
    lv_obj_set_size(clear, 64, 28);
    lv_obj_align(clear, LV_ALIGN_TOP_RIGHT, -4, 0);
    lv_obj_set_style_radius(clear, 6, 0);
    lv_obj_set_style_shadow_width(clear, 0, 0);
    lv_obj_add_event_cb(clear, clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(clear);
    lv_label_set_text(cl, "清空");
    lv_obj_set_style_text_font(cl, fw_asset_font_20(), 0);
    lv_obj_center(cl);

    s_list = fw_ui_list(panel, NULL);
    lv_obj_set_size(s_list, lv_disp_get_hor_res(NULL) - 12, NC_PANEL_H - 48);
    lv_obj_align(s_list, LV_ALIGN_BOTTOM_MID, 0, 0);

    rebuild();

    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    lvgl_port_unlock();

    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_POSTED, evt_posted, NULL);
    svc_event_bus_subscribe(SVC_EVENT_NOTIFICATION_DISMISSED, evt_dismissed, NULL);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t fw_notification_show(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    rebuild();
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
