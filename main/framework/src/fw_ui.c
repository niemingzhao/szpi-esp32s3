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
#define FW_DIALOG_BTN_GAP   8     /* 对话框按钮之间的间距 */

typedef struct {
    lv_obj_t *scrim;
    fw_dialog_cb_t cb;
    void *user;
} fw_dialog_state_t;

static fw_dialog_state_t s_dialog;

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
    lv_obj_set_scrollable(box, false);
    lv_obj_set_style_bg_color(box, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(box, 12, 0);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, title != NULL ? title : "");
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
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

/* 异步删除真正发生后才清状态：这样 close 到删除之间不会叠加第二个对话框 */
static void dialog_scrim_deleted_cb(lv_event_t *e)
{
    lv_obj_t *scrim = lv_event_get_target(e);
    if (s_dialog.scrim == scrim) {
        s_dialog.scrim = NULL;
        s_dialog.cb = NULL;
        s_dialog.user = NULL;
    }
}

static void dialog_btn_cb(lv_event_t *e)
{
    /* 按钮 ID 按值随事件带过来，避免被下一个对话框覆盖共享数组 */
    fw_dialog_btn_t btn = (fw_dialog_btn_t)(intptr_t)lv_event_get_user_data(e);

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
        ESP_LOGW(TAG, "dialog already open (wait until it is deleted)");
        return NULL;
    }

    lvgl_port_lock(0);

    lv_obj_t *scrim = lv_obj_create(parent != NULL ? parent : lv_layer_top());
    lv_obj_set_size(scrim, lv_display_get_horizontal_resolution(lv_display_get_default()), lv_display_get_vertical_resolution(lv_display_get_default()));
    lv_obj_add_event_cb(scrim, dialog_scrim_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_scrollable(scrim, false);
    lv_obj_set_clickable(scrim, true);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_set_size(panel, 268, 156);
    lv_obj_center(panel);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_style_bg_color(panel, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_set_style_text_font(panel, fw_asset_font_cn(), 0);

    if (title != NULL) {
        lv_obj_t *l = lv_label_create(panel);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_font(l, fw_asset_font_cn_large(), 0);
        lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    if (msg != NULL) {
        lv_obj_t *m = lv_label_create(panel);
        lv_label_set_text(m, msg);
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(m, 244);
        lv_obj_set_style_text_font(m, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(m, fw_theme_color_text_secondary(), 0);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 34);
    }

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, 240, 36);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, FW_DIALOG_BTN_GAP, 0);   /* 按钮之间的间距 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 按钮在行内均分（先数出个数，宽度写死，不依赖 flex grow 的剩余空间算法） */
    uint8_t total = 0;
    for (size_t i = 0; i < sizeof(k_btn_order) / sizeof(k_btn_order[0]); i++) {
        if ((buttons & k_btn_order[i]) == 0) continue;
        if (total >= FW_DIALOG_BTN_MAX) break;
        total++;
    }
    if (total == 0) total = 1;
    const lv_coord_t btn_w =
        (240 - (lv_coord_t)(total - 1) * FW_DIALOG_BTN_GAP) / (lv_coord_t)total;

    uint8_t n = 0;
    for (size_t i = 0; i < sizeof(k_btn_order) / sizeof(k_btn_order[0]); i++) {
        if ((buttons & k_btn_order[i]) == 0) continue;
        if (n >= FW_DIALOG_BTN_MAX) break;

        bool primary = (k_btn_order[i] == FW_DIALOG_BTN_OK);

        /* 形状与状态栏返回 / 主页按钮一致（圆角卡片 + 描边），宽度按行内均分 */
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_size(btn, btn_w, 34);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        /* 显式设色：不能依赖 LVGL 自带主题（它只在启动时定一次） */
        lv_obj_set_style_bg_color(btn, primary ? fw_theme_color_accent() : fw_theme_color_bg_card(), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, primary ? fw_theme_color_accent() : fw_theme_color_border(), 0);
        lv_obj_set_style_text_font(btn, fw_asset_font_cn(), 0);
        lv_obj_add_event_cb(btn, dialog_btn_cb, LV_EVENT_SHORT_CLICKED,
                            (void *)(intptr_t)k_btn_order[i]);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, btn_text(k_btn_order[i]));
        lv_obj_set_style_text_color(bl, primary ? lv_color_white() : fw_theme_color_text_primary(), 0);
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
        /* 可能是在对话框自己的按钮回调里调用的，不能当场删活动对象；
         * 状态留给 dialog_scrim_deleted_cb 清，删除生效前不允许再开新对话框 */
        lv_obj_delete_async(s_dialog.scrim);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

/* ---------------------------------- Toast --------------------------------- */

/*
 * 同一时刻只保留一个 Toast：新 Toast 顶掉旧的。
 *
 * 句柄与它的一次性定时器都放在模块内，删除时自己清。这里**不用** lv_obj_is_valid()
 * 判断对象是否还在 —— LVGL 9.6 里它只是 lv_obj_is_in_widget_tree 的别名
 * （lv_api_map_v9_5.h），会顺着 obj->parent 往上走，对已经释放的对象解引用会
 * 直接崩溃（GCC 也不会提醒，因为它只是宏）。
 * 定时器也不能把对象指针存在 user_data 里：对象被外部删掉后它就是悬空指针。
 */
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;

/* 外部把 Toast 删掉时兜底：只清句柄，定时器由 toast_dismiss() / 到期回调处理 */
static void toast_deleted_cb(lv_event_t *e)
{
    if (s_toast == lv_event_get_target(e)) {
        s_toast = NULL;
    }
}

/* 关掉当前 Toast 与它的定时器；调用者必须已持 LVGL 锁 */
static void toast_dismiss(void)
{
    if (s_toast_timer != NULL) {
        lv_timer_delete(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

static void toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 一次性定时器：这个回调返回后 LVGL 就会把它删掉，先把句柄清掉 */
    s_toast_timer = NULL;
    if (s_toast != NULL) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

lv_obj_t *fw_ui_toast(const char *msg, uint32_t duration_ms)
{
    if (msg == NULL) return NULL;
    if (duration_ms == 0) duration_ms = 3000;

    lvgl_port_lock(0);

    toast_dismiss();        /* 顶掉上一个 */

    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast, 272, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(toast, false);
    lv_obj_set_style_bg_color(toast, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, 8, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_border_color(toast, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(toast, 12, 0);

    lv_obj_t *label = lv_label_create(toast);
    lv_label_set_text(label, msg);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 248);
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_center(label);

    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -44);

    lv_obj_add_event_cb(toast, toast_deleted_cb, LV_EVENT_DELETE, NULL);
    s_toast = toast;
    s_toast_timer = lv_timer_create(toast_timer_cb, duration_ms, NULL);
    if (s_toast_timer != NULL) {
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }

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
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);

    if (title != NULL) {
        lv_obj_t *l = lv_label_create(box);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    }

    lvgl_port_unlock();
    return box;
}

lv_obj_t *fw_ui_list_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *user)
{
    if (list == NULL) return NULL;

    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 38);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text != NULL ? text : "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, lv_pct(88));
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
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
    lv_obj_set_scrollable(g, false);
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

/* ------------------------------ App 页面常用件 ------------------------------ */

lv_obj_t *fw_ui_page(lv_obj_t **content)
{
    lvgl_port_lock(0);

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_pad_all(root, 0, 0);

    /* 内容区从状态栏下方开始（与 Settings / 浮层一致） */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_size(body, lv_pct(100), lv_display_get_vertical_resolution(lv_display_get_default()) - FW_STATUSBAR_H);
    lv_obj_set_pos(body, 0, FW_STATUSBAR_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (content != NULL) *content = body;

    lvgl_port_unlock();
    return root;
}

lv_obj_t *fw_ui_row_btn(lv_obj_t *parent, const char *symbol, const char *text,
                        lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 50);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    if (symbol != NULL) {
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, fw_asset_font_20(), 0);
        lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(58));
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 44, 0);

    /* 右侧数值文本：先建好（空），由 fw_ui_row_btn_value() 更新；用 USER_1 标记便于查找 */
    lv_obj_t *value = lv_label_create(btn);
    lv_label_set_text(value, "");
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(value, lv_pct(34));
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(value, fw_theme_color_text_secondary(), 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_user_data(value, (void *)1);

    lvgl_port_unlock();
    return btn;
}

esp_err_t fw_ui_row_btn_value(lv_obj_t *btn, const char *value)
{
    if (btn == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);

    uint32_t n = lv_obj_get_child_cnt(btn);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, (int32_t)i);
        if (lv_obj_get_user_data(child) != NULL) {
            lv_label_set_text(child, value != NULL ? value : "");
            lvgl_port_unlock();
            return ESP_OK;
        }
    }

    lvgl_port_unlock();
    return ESP_ERR_NOT_FOUND;
}

lv_obj_t *fw_ui_slider_row(lv_obj_t *parent, const char *label, int32_t min, int32_t max,
                           int32_t value, lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label != NULL ? label : "");
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *bar = lv_slider_create(row);
    lv_obj_set_width(bar, lv_pct(78));
    lv_obj_set_height(bar, 16);
    lv_obj_align(bar, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_slider_set_range(bar, min, max);
    lv_slider_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, fw_theme_color_bg_secondary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, fw_theme_color_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, fw_theme_color_accent(), LV_PART_KNOB);
    lv_obj_set_style_border_width(bar, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(bar, fw_theme_color_border(), LV_PART_KNOB);
    if (cb != NULL) lv_obj_add_event_cb(bar, cb, LV_EVENT_VALUE_CHANGED, user);

    lvgl_port_unlock();
    return bar;
}
