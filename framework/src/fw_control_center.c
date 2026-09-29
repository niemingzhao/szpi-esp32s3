/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Control Center 实现
 *
 * 由状态栏的“控制中心”按钮打开，占满状态栏以下的显示区；内容用纵向 flex 排布，
 * 超出高度时可滚动。右上角 × 关闭。
 * Wi-Fi 与蓝牙磁贴整行可点（蓝牙控制广播开关）；手电筒 / 锁屏磁贴待对应硬件与功能落地后再补。
 */

#include "fw_control_center.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "fw.ctrl_center";

static lv_obj_t *s_scrim = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_bt_label = NULL;
static lv_obj_t *s_torch_label = NULL;
static bool s_torch_on = false;
static uint8_t s_torch_saved = 80;
static lv_obj_t *s_bright = NULL;
static lv_obj_t *s_vol = NULL;
static bool s_visible = false;
static bool s_bright_sync = false;   /* 事件回写滑块时抑制 VALUE_CHANGED 再次发布 */

static void update_wifi(void)
{
    if (s_wifi_label == NULL) return;

    svc_net_status_t st;
    bool connected = (svc_net_get_status(&st) == ESP_OK) && st.wifi_connected;

    lv_label_set_text(s_wifi_label, connected ? (LV_SYMBOL_WIFI "  已连接")
                                              : (LV_SYMBOL_WIFI "  未连接"));
}

/* 蓝牙磁贴状态文字：未启动 / 未广播 / 广播中（可配对）/ 已连接 */
static void update_bt(void)
{
    if (s_bt_label == NULL) return;

    const char *text;
    switch (svc_bt_get_state()) {
    case SVC_BT_STATE_CONNECTED:
        text = LV_SYMBOL_BLUETOOTH "  已连接";
        break;
    case SVC_BT_STATE_ADVERTISING:
        text = LV_SYMBOL_BLUETOOTH "  广播中";
        break;
    case SVC_BT_STATE_READY:
        text = LV_SYMBOL_BLUETOOTH "  未广播";
        break;
    default:
        text = LV_SYMBOL_BLUETOOTH "  未启动";
        break;
    }
    lv_label_set_text(s_bt_label, text);
}

/* 蓝牙磁贴：开关广播（协议栈在 Services 初始化早期启动，这里只切换可被发现） */
static void bt_cb(lv_event_t *e)
{
    (void)e;

    switch (svc_bt_get_state()) {
    case SVC_BT_STATE_ADVERTISING:
        svc_bt_adv_stop();
        break;
    case SVC_BT_STATE_READY:
        svc_bt_adv_start();
        break;
    default:                        /* OFF / CONNECTED：不改广播 */
        break;
    }
    update_bt();
}

static void evt_bt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    lvgl_port_lock(0);
    update_bt();
    lvgl_port_unlock();
}

/* ------------------------------- 手电筒 ------------------------------- */

/* 本板没有可控 LED（01-hardware-spec.md 里唯一的"灯"是 LCD 背光），
 * 所以"手电筒"做成屏幕手电筒：背光拉到 100%，关闭时恢复原来的亮度。 */
static void update_torch(void)
{
    if (s_torch_label == NULL) return;
    lv_label_set_text(s_torch_label, s_torch_on ? (LV_SYMBOL_TINT "  手电筒  开")
                                                : (LV_SYMBOL_TINT "  手电筒  关"));
}

static void torch_cb(lv_event_t *e)
{
    (void)e;

    if (s_torch_on) {
        svc_power_set_brightness(s_torch_saved > 0 ? s_torch_saved : 80);
        s_torch_on = false;
    } else {
        s_torch_saved = svc_power_get_brightness();
        svc_power_set_brightness(100);
        s_torch_on = true;
    }
    update_torch();
}

/* 锁定：立刻锁屏（先把控制中心收起来） */
static void lock_cb(lv_event_t *e)
{
    (void)e;
    fw_control_center_hide();
    fw_lockscreen_lock();
}

static void forget_confirmed(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    svc_net_wifi_forget();
    update_wifi();
    fw_ui_toast("已忘记网络", 2000);
}

static void wifi_long_press_cb(lv_event_t *e)
{
    (void)e;
    fw_ui_dialog(NULL, "Wi-Fi", "忘记已保存的网络？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, forget_confirmed, NULL);
}

static void scrim_cb(lv_event_t *e)
{
    (void)e;
    fw_control_center_hide();
}

static void wifi_cb(lv_event_t *e)
{
    (void)e;

    svc_net_status_t st;
    bool conn = (svc_net_get_status(&st) == ESP_OK) && st.wifi_connected;

    if (conn) {
        svc_net_wifi_stop();                 /* 断开（取消连接） */
        fw_ui_toast("Wi-Fi 已断开", 1500);
    } else {
        /* 免密重连已记住的网络；没保存过就只打开 Wi-Fi 开关 */
        esp_err_t err = svc_net_wifi_auto_connect();
        if (err == ESP_ERR_NOT_FOUND) {
            svc_net_wifi_start(SVC_NET_MODE_STA);
            fw_ui_toast("没有已保存的网络", 2000);
        } else if (err != ESP_OK) {
            fw_ui_toast(esp_err_to_name(err), 2000);
        } else {
            fw_ui_toast("正在连接已保存的网络…", 2000);
        }
    }
    update_wifi();
}

static void bright_cb(lv_event_t *e)
{
    if (s_bright_sync) return;
    lv_obj_t *s = lv_event_get_target(e);
    svc_power_set_brightness((uint8_t)lv_slider_get_value(s));
}

/* 亮度被别处（如 Settings）改动时保持同步 */
static void evt_brightness(const svc_event_t *evt, void *user)
{
    (void)user;
    if (s_bright == NULL || evt->data == NULL || evt->data_len < sizeof(uint8_t)) return;

    uint8_t v = *(const uint8_t *)evt->data;
    lvgl_port_lock(0);
    s_bright_sync = true;
    lv_slider_set_value(s_bright, (int32_t)v, LV_ANIM_OFF);
    s_bright_sync = false;
    lvgl_port_unlock();
}

static void vol_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    svc_audio_set_volume((uint8_t)lv_slider_get_value(s));
}

static void audition_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = svc_audio_play_tone_async(1000, 300);
    if (err != ESP_OK) {
        fw_ui_toast("音频不可用", 2000);
    }
}

static void evt_wifi(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;
    lvgl_port_lock(0);
    update_wifi();
    lvgl_port_unlock();
}

/* 一行“标签 + 滑块”，滑块自动占满剩余宽度 */
static lv_obj_t *make_slider_row(lv_obj_t *parent, const char *text, int32_t value,
                                lv_event_cb_t cb, lv_obj_t **slider_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 30);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 40);
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_secondary(), 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 16);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    /* 显式设色：轨道用边框色、指示条与圆点用主色 */
    lv_obj_set_style_bg_color(slider, fw_theme_color_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, fw_theme_color_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, fw_theme_color_accent(), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);

    *slider_out = slider;
    return row;
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

    /* 面板：固定标题 + 分隔线 + 可滚动内容区（与通知中心观感一致） */
    lv_obj_t *panel = lv_obj_create(s_scrim);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* 标题行（与通知中心完全一致的定位） */
    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "控制中心");
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
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(cl, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(cl, fw_theme_color_text_primary(), 0);
    lv_obj_center(cl);

    /* 分隔线（与通知中心一致：左右各留 12） */
    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_set_size(line, w - 24, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(line, fw_theme_color_divider(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    /* 内容区：起点与通知中心列表一致，纵向 flex，超出高度可滚动 */
    lv_obj_t *body = lv_obj_create(panel);
    lv_obj_set_size(body, w, h - 50);
    lv_obj_set_pos(body, 0, 50);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    /* Wi-Fi：整行可点，长按清除凭据 */
    lv_obj_t *wifi = lv_btn_create(body);
    lv_obj_set_width(wifi, lv_pct(100));
    lv_obj_set_height(wifi, 46);
    lv_obj_set_style_bg_color(wifi, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(wifi, 8, 0);
    lv_obj_set_style_shadow_width(wifi, 0, 0);
    lv_obj_set_style_border_width(wifi, 1, 0);
    lv_obj_set_style_border_color(wifi, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(wifi, wifi_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(wifi, wifi_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_wifi_label = lv_label_create(wifi);
    lv_obj_set_style_text_font(s_wifi_label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_wifi_label, fw_theme_color_text_primary(), 0);
    lv_obj_center(s_wifi_label);

    /* 蓝牙：整行可点，开关广播 */
    lv_obj_t *bt = lv_btn_create(body);
    lv_obj_set_width(bt, lv_pct(100));
    lv_obj_set_height(bt, 46);
    lv_obj_set_style_bg_color(bt, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(bt, 8, 0);
    lv_obj_set_style_shadow_width(bt, 0, 0);
    lv_obj_set_style_border_width(bt, 1, 0);
    lv_obj_set_style_border_color(bt, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(bt, bt_cb, LV_EVENT_SHORT_CLICKED, NULL);
    s_bt_label = lv_label_create(bt);
    lv_obj_set_style_text_font(s_bt_label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_bt_label, fw_theme_color_text_primary(), 0);
    lv_obj_center(s_bt_label);
    update_bt();

    /* 亮度 / 音量 */
    make_slider_row(body, "亮度", (int32_t)svc_power_get_brightness(), bright_cb, &s_bright);
    make_slider_row(body, "音量", (int32_t)svc_audio_get_volume(), vol_cb, &s_vol);

    /* 手电筒：屏幕手电筒（本板无独立 LED，用背光代替） */
    lv_obj_t *torch = lv_btn_create(body);
    lv_obj_set_width(torch, lv_pct(100));
    lv_obj_set_height(torch, 46);
    lv_obj_set_style_bg_color(torch, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(torch, 8, 0);
    lv_obj_set_style_shadow_width(torch, 0, 0);
    lv_obj_set_style_border_width(torch, 1, 0);
    lv_obj_set_style_border_color(torch, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(torch, torch_cb, LV_EVENT_SHORT_CLICKED, NULL);
    s_torch_label = lv_label_create(torch);
    lv_obj_set_style_text_font(s_torch_label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_torch_label, fw_theme_color_text_primary(), 0);
    lv_obj_center(s_torch_label);
    update_torch();

    /* 锁定：整行可点，锁屏界面由 fw_lockscreen 提供 */
    lv_obj_t *lock = lv_btn_create(body);
    lv_obj_set_width(lock, lv_pct(100));
    lv_obj_set_height(lock, 46);
    lv_obj_set_style_bg_color(lock, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(lock, 8, 0);
    lv_obj_set_style_shadow_width(lock, 0, 0);
    lv_obj_set_style_border_width(lock, 1, 0);
    lv_obj_set_style_border_color(lock, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(lock, lock_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *ll = lv_label_create(lock);
    lv_label_set_text(ll, "锁定");
    lv_obj_set_style_text_font(ll, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(ll, fw_theme_color_text_primary(), 0);
    lv_obj_center(ll);

    /* 试听：验证音频通路（ES8311 + I2S + 功放） */
    lv_obj_t *audition = lv_btn_create(body);
    lv_obj_set_size(audition, 120, 36);
    lv_obj_set_style_bg_color(audition, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(audition, 8, 0);
    lv_obj_set_style_shadow_width(audition, 0, 0);
    lv_obj_set_style_border_width(audition, 1, 0);
    lv_obj_set_style_border_color(audition, fw_theme_color_border(), 0);
    lv_obj_add_event_cb(audition, audition_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(audition);
    lv_label_set_text(al, "试听");
    lv_obj_set_style_text_font(al, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(al, fw_theme_color_text_primary(), 0);
    lv_obj_center(al);
}

esp_err_t fw_control_center_init(void)
{
    lvgl_port_lock(0);
    panel_build(lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL) - FW_STATUSBAR_H);
    update_wifi();
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    lvgl_port_unlock();

    svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_WIFI_DISCONNECTED, evt_wifi, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, evt_bt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BRIGHTNESS_CHANGED, evt_brightness, NULL);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

/* 换主题时重建全部控件（配色写死在控件上，重建最省事） */
esp_err_t fw_control_center_rebuild(void)
{
    lvgl_port_lock(0);

    if (s_scrim != NULL) {
        lv_obj_del(s_scrim);
        s_scrim = NULL;
        s_wifi_label = NULL;
        s_bt_label = NULL;
        s_torch_label = NULL;
        s_bright = NULL;
        s_vol = NULL;
    }

    panel_build(lv_disp_get_hor_res(NULL), lv_disp_get_ver_res(NULL) - FW_STATUSBAR_H);
    update_wifi();
    if (s_visible) lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_control_center_show(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    update_wifi();
    update_bt();
    lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_control_center_hide(void)
{
    if (s_scrim == NULL) return ESP_ERR_INVALID_STATE;

    lvgl_port_lock(0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_control_center_toggle(void)
{
    return s_visible ? fw_control_center_hide() : fw_control_center_show();
}

bool fw_control_center_is_visible(void)
{
    return s_visible;
}
