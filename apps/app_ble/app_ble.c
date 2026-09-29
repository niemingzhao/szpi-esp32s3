/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - BLE HID 模拟器（APP-BLE）
 *
 * 把本机当 BLE 键盘 / 媒体遥控器用：上方显示连接状态，下面的按钮通过 svc_bt_hid
 * 发送 HID 报告（媒体键走消费类报告，方向键走键盘报告）。协议栈在 Services
 * 初始化早期启动（见 AGENTS 4.6），配对由主机侧发起（Just Works）。
 */

#include "app_ble.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "app.ble";

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_status = NULL;

/* --------------------------------- 状态行 --------------------------------- */

static void update_status(void)
{
    if (s_status == NULL) return;

    const char *text;
    switch (svc_bt_get_state()) {
    case SVC_BT_STATE_CONNECTED:
        text = "已连接";
        break;
    case SVC_BT_STATE_ADVERTISING:
        text = "可配对：广播中";
        break;
    case SVC_BT_STATE_READY:
        text = "准备中";
        break;
    default:
        text = "未启动";       /* OFF 或未知状态 */
        break;
    }
    lv_label_set_text(s_status, text);
}

static void evt_bt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    lvgl_port_lock(0);
    update_status();
    lvgl_port_unlock();
}

/* --------------------------------- 按钮 --------------------------------- */

static void consumer_cb(lv_event_t *e)
{
    uint8_t usage = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (svc_bt_hid_consumer_click(usage) != ESP_OK) {
        fw_ui_toast("未连接", 1500);
    }
}

static void keyboard_cb(lv_event_t *e)
{
    uint8_t usage = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (svc_bt_hid_key_click(usage, 0) != ESP_OK) {
        fw_ui_toast("未连接", 1500);
    }
}

/* 一个方形图标按钮 */
static void add_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, uint8_t usage)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 64, 44);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)usage);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_center(label);
}

/* --------------------------------- 生命周期 --------------------------------- */

static void *ble_on_create(void)
{
    lvgl_port_lock(0);

    s_root = lv_obj_create(NULL);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_root, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);

    /* 内容从状态栏下方开始（与 Settings 一致）；按钮总高略超可视区，body 允许纵向滚动 */
    lv_obj_t *body = lv_obj_create(s_root);
    lv_obj_set_size(body, lv_pct(100), lv_disp_get_ver_res(NULL) - FW_STATUSBAR_H);
    lv_obj_set_pos(body, 0, FW_STATUSBAR_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_status = lv_label_create(body);
    lv_obj_set_style_text_font(s_status, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_status, fw_theme_color_text_primary(), 0);

    lv_obj_t *hint = lv_label_create(body);
    lv_label_set_text(hint, "设备 SZPI-OS，配对后可发送按键");
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);

    /* 媒体键：音量减 / 播放暂停 / 音量加 */
    lv_obj_t *media = lv_obj_create(body);
    lv_obj_set_size(media, lv_pct(100), 48);
    lv_obj_clear_flag(media, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(media, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(media, 0, 0);
    lv_obj_set_style_pad_all(media, 0, 0);
    lv_obj_set_style_pad_column(media, 12, 0);
    lv_obj_set_flex_flow(media, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(media, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_btn(media, LV_SYMBOL_VOLUME_MID, consumer_cb, SVC_BT_HID_CONSUMER_VOL_DOWN);
    add_btn(media, LV_SYMBOL_PLAY, consumer_cb, SVC_BT_HID_CONSUMER_PLAY_PAUSE);
    add_btn(media, LV_SYMBOL_VOLUME_MAX, consumer_cb, SVC_BT_HID_CONSUMER_VOL_UP);

    /* 方向键 */
    lv_obj_t *keys = lv_obj_create(body);
    lv_obj_set_size(keys, lv_pct(100), 96);
    lv_obj_clear_flag(keys, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(keys, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keys, 0, 0);
    lv_obj_set_style_pad_all(keys, 0, 0);
    lv_obj_set_style_pad_column(keys, 12, 0);
    lv_obj_set_style_pad_row(keys, 8, 0);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(keys, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    add_btn(keys, LV_SYMBOL_UP, keyboard_cb, SVC_BT_HID_KEY_UP_ARROW);
    add_btn(keys, LV_SYMBOL_DOWN, keyboard_cb, SVC_BT_HID_KEY_DOWN_ARROW);
    add_btn(keys, LV_SYMBOL_LEFT, keyboard_cb, SVC_BT_HID_KEY_LEFT_ARROW);
    add_btn(keys, LV_SYMBOL_RIGHT, keyboard_cb, SVC_BT_HID_KEY_RIGHT_ARROW);
    add_btn(keys, LV_SYMBOL_OK, keyboard_cb, SVC_BT_HID_KEY_ENTER);
    add_btn(keys, LV_SYMBOL_CLOSE, keyboard_cb, SVC_BT_HID_KEY_ESCAPE);

    update_status();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void ble_on_start(void *ctx)
{
    (void)ctx;
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, evt_bt, NULL);

    lvgl_port_lock(0);
    update_status();
    lvgl_port_unlock();
}

static void ble_on_pause(void *ctx)
{
    (void)ctx;
    svc_event_bus_unsubscribe(SVC_EVENT_BT_STATE_CHANGED, evt_bt);
}

static void ble_on_resume(void *ctx)
{
    (void)ctx;
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, evt_bt, NULL);

    lvgl_port_lock(0);
    update_status();
    lvgl_port_unlock();
}

static void ble_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
        s_status = NULL;
    }
    lvgl_port_unlock();
}

const fw_app_desc_t app_ble_desc = {
    .name = "BLE",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_BLUETOOTH,
    .on_create = ble_on_create,
    .on_start = ble_on_start,
    .on_pause = ble_on_pause,
    .on_resume = ble_on_resume,
    .on_destroy = ble_on_destroy,
};
