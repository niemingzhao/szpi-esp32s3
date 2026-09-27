/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - UI 通用组件实现
 */

#include "fw_ui.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "fw.ui";

#define FW_DIALOG_BTN_MAX   4
#define FW_DIALOG_BTN_GAP   8     /* 对话框按钮之间的间距 */
#define FW_DIALOG_PANEL_W   268   /* 对话框宽度（320 屏宽留出两侧边距） */
#define FW_DIALOG_PAD       12    /* 对话框内边距 */

#define FW_UI_STAGE_PERIOD_MS  16 /* 分段构建的间隔：约一帧建一批 */

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
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, 216);           /* 内宽：超长截断，别顶出卡片 */
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

esp_err_t fw_ui_progress_title(lv_obj_t *bar, const char *text)
{
    if (bar == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);
    /* 子对象顺序固定：0 = 标题 label，1 = lv_bar */
    lv_obj_t *label = lv_obj_get_child(bar, 0);
    if (label != NULL) {
        lv_label_set_text(label, text != NULL ? text : "");
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
    lv_obj_set_width(panel, FW_DIALOG_PANEL_W);
    /* 高度跟着内容走（标题 + 正文 + 按钮行），正文长也不会压到按钮底下；
     * 再长则在本屏内截住，不越出屏幕 */
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, 228, 0);
    lv_obj_center(panel);
    lv_obj_set_scrollable(panel, false);
    lv_obj_set_style_bg_color(panel, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(panel, FW_DIALOG_PAD, 0);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_set_style_text_font(panel, fw_asset_font_cn(), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (title != NULL) {
        lv_obj_t *l = lv_label_create(panel);
        lv_label_set_text(l, title);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_font(l, fw_asset_font_cn_large(), 0);
        lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    }

    if (msg != NULL) {
        lv_obj_t *m = lv_label_create(panel);
        lv_label_set_text(m, msg);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(m, lv_pct(100));
        lv_obj_set_style_text_font(m, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(m, fw_theme_color_text_secondary(), 0);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, lv_pct(100), 36);
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
        total++;
    }
    if (total > FW_DIALOG_BTN_MAX) {
        ESP_LOGW(TAG, "too many dialog buttons, %u of %u shown", FW_DIALOG_BTN_MAX, total);
        total = FW_DIALOG_BTN_MAX;
    }
    if (total == 0) total = 1;
    const int32_t btn_w =
        (FW_DIALOG_PANEL_W - 2 * FW_DIALOG_PAD - (int32_t)(total - 1) * FW_DIALOG_BTN_GAP) /
        (int32_t)total;

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
 * 句柄与它的一次性定时器都放在模块内，删除时自己清。这里不用 lv_obj_is_in_widget_tree()
 * 判断对象是否还在 —— LVGL 9.6 里它只是 lv_obj_is_in_widget_tree 的别名，
 * 会顺着 obj->parent 往上走，对已经释放的对象解引用会直接崩溃
 * （GCC 也不会提醒，因为它只是宏）。
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
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
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

static lv_obj_t *list_item_build(lv_obj_t *list, const lv_image_dsc_t *icon, const char *text,
                                 const char *value, lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(list);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 40);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    /* 浅色主题下 bg_secondary 与卡片同为白色，靠 1 px 分隔色描边区分出列表项 */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_divider(), 0);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    const int32_t text_x = (icon != NULL) ? 34 : 8;

    if (icon != NULL) {
        lv_obj_t *ic = lv_image_create(btn);
        lv_image_set_src(ic, icon);
        lv_obj_set_style_image_recolor(ic, fw_theme_color_accent(), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);
    }

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text != NULL ? text : "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, lv_pct((value != NULL) ? 56 : 82));
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, text_x, 0);

    if (value != NULL) {
        lv_obj_t *v = lv_label_create(btn);
        lv_label_set_text(v, value);
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(v, lv_pct(34));
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(v, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(v, fw_theme_color_text_secondary(), 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_user_data(v, (void *)1);
    }

    lvgl_port_unlock();
    return btn;
}

lv_obj_t *fw_ui_list_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *user)
{
    if (list == NULL) return NULL;
    return list_item_build(list, NULL, text, NULL, cb, user);
}

lv_obj_t *fw_ui_list_add_value(lv_obj_t *list, const char *text, const char *value,
                               lv_event_cb_t cb, void *user)
{
    if (list == NULL) return NULL;
    return list_item_build(list, NULL, text, value, cb, user);
}

lv_obj_t *fw_ui_list_add_icon(lv_obj_t *list, const lv_image_dsc_t *icon, const char *text,
                              const char *value, lv_event_cb_t cb, void *user)
{
    if (list == NULL) return NULL;
    return list_item_build(list, icon, text, value, cb, user);
}

lv_obj_t *fw_ui_list_hint(lv_obj_t *list, const char *text)
{
    if (list == NULL) return NULL;

    lvgl_port_lock(0);

    lv_obj_t *lb = lv_label_create(list);
    lv_label_set_text(lb, text != NULL ? text : "");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_secondary(), 0);

    lvgl_port_unlock();
    return lb;
}

esp_err_t fw_ui_list_value_color(lv_obj_t *item, lv_color_t color)
{
    if (item == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    uint32_t n = lv_obj_get_child_count(item);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
        if (lv_obj_get_user_data(child) != NULL) {
            lv_obj_set_style_text_color(child, color, 0);
            ret = ESP_OK;
            break;
        }
    }

    lvgl_port_unlock();
    return ret;
}

esp_err_t fw_ui_list_mark(lv_obj_t *item, bool on)
{
    if (item == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);

    lv_obj_set_style_border_color(item,
                                  on ? fw_theme_color_accent() : fw_theme_color_divider(), 0);

    /* 文字标签不能按"第 0 个子对象"取：带图标的列表项第 0 个是图标，
     * 按类型找第一个文本标签更稳（右侧数值带 user_data 标记，跳过） */
    uint32_t n = lv_obj_get_child_count(item);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
        if (lv_obj_get_user_data(child) == NULL && lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_style_text_color(child,
                                        on ? fw_theme_color_accent() : fw_theme_color_text_primary(), 0);
            break;
        }
    }

    lvgl_port_unlock();
    return ESP_OK;
}

/* ---------------------------------- 网格 ---------------------------------- */

lv_obj_t *fw_ui_grid(lv_obj_t *parent, uint8_t cols, int32_t item_w, int32_t item_h)
{
    (void)item_h;
    if (cols == 0) cols = 1;

    lvgl_port_lock(0);

    lv_obj_t *g = lv_obj_create(parent != NULL ? parent : lv_layer_top());
    lv_obj_set_size(g, (int32_t)(cols * item_w + (cols - 1) * 8 + 12), LV_SIZE_CONTENT);
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

/* 行入口的公共部分：左侧图标（symbol 或 img 二选一）+ 文本 + 右侧数值 */
static lv_obj_t *row_btn_build(lv_obj_t *parent, const char *symbol,
                               const lv_image_dsc_t *img, const char *text,
                               lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 44);
    lv_obj_set_scrollable(btn, false);
    /* 设置项做成"扁平行"而不是卡片：透明底 + 底部 1 px 分隔线，
     * 一页设置看起来是一份列表，不会变成一叠卡片（卡片只留给内容块） */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_divider(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    if (img != NULL) {
        lv_obj_t *icon = lv_image_create(btn);
        lv_image_set_src(icon, img);
        lv_obj_set_style_image_recolor(icon, fw_theme_color_accent(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    } else if (symbol != NULL) {
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, fw_asset_font_20(), 0);
        lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text != NULL ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, lv_pct(58));
    lv_obj_set_style_text_font(label, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(label, fw_theme_color_text_primary(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 44, 0);

    /* 右侧数值文本：先建好（空），由 fw_ui_row_btn_value() 更新；用 user_data 标记便于查找 */
    lv_obj_t *value = lv_label_create(btn);
    lv_label_set_text(value, "");
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(value, lv_pct(34));
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(value, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(value, fw_theme_color_text_secondary(), 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_user_data(value, (void *)1);

    lvgl_port_unlock();
    return btn;
}

lv_obj_t *fw_ui_row_btn(lv_obj_t *parent, const char *symbol, const char *text,
                        lv_event_cb_t cb, void *user)
{
    return row_btn_build(parent, symbol, NULL, text, cb, user);
}

lv_obj_t *fw_ui_row_btn_img(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text,
                            lv_event_cb_t cb, void *user)
{
    return row_btn_build(parent, NULL, icon, text, cb, user);
}

/* ---------------------------------- 卡片 / 分组 ---------------------------------- */

static lv_obj_t *card_build(lv_obj_t *parent, int32_t h, bool hero)
{
    lvgl_port_lock(0);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), h);
    lv_obj_set_style_bg_color(card, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_border_width(card, hero ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, hero ? fw_theme_color_accent()
                                             : fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_scrollable(card, false);

    lvgl_port_unlock();
    return card;
}

lv_obj_t *fw_ui_card(lv_obj_t *parent, int32_t h)
{
    return card_build(parent, h, false);
}

lv_obj_t *fw_ui_hero_card(lv_obj_t *parent, int32_t h)
{
    return card_build(parent, h, true);
}

lv_obj_t *fw_ui_group(lv_obj_t *parent)
{
    lvgl_port_lock(0);

    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_width(g, lv_pct(100));
    lv_obj_set_height(g, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(g, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(g, 12, 0);
    lv_obj_set_style_shadow_width(g, 0, 0);
    lv_obj_set_style_border_width(g, 1, 0);
    lv_obj_set_style_border_color(g, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_style_pad_row(g, 0, 0);
    lv_obj_set_scrollable(g, false);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lvgl_port_unlock();
    return g;
}

esp_err_t fw_ui_group_end(lv_obj_t *group)
{
    if (group == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);

    const uint32_t n = lv_obj_get_child_count(group);
    if (n > 0) {
        lv_obj_t *last = lv_obj_get_child(group, (int32_t)(n - 1));
        if (last != NULL) {
            lv_obj_set_style_border_width(last, 0, 0);
        }
    }

    lvgl_port_unlock();
    return ESP_OK;
}

lv_obj_t *fw_ui_gap(lv_obj_t *parent, int32_t h)
{
    lvgl_port_lock(0);

    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, 1, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);

    lvgl_port_unlock();
    return o;
}

/* ------------------------------- 详情格 ------------------------------- */

#define TBL_CELL_H    22
#define TBL_LINE      1

lv_obj_t *fw_ui_table(lv_obj_t *parent, uint8_t cols, uint8_t rows, const char *const *labels)
{
    if (cols == 0 || rows == 0) return NULL;

    lvgl_port_lock(0);

    /* 容器底色用 divider 当表格线，行 / 列之间留 1 px 透出来；容器自己 1 px 描边做外框，
     * clip_corner 让圆角裁掉格子的直角。一行用一个 ROW 容器包住 —— 把格子直接塞进
     * ROW_WRAP 会让换行按 0 宽去算，结果全挤成一行 */
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_set_size(grid, lv_pct(100), rows * TBL_CELL_H + (rows - 1) * TBL_LINE + 2 * TBL_LINE);
    lv_obj_set_style_bg_color(grid, fw_theme_color_divider(), 0);
    lv_obj_set_style_border_width(grid, TBL_LINE, 0);
    lv_obj_set_style_border_color(grid, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(grid, 12, 0);
    lv_obj_set_style_clip_corner(grid, true, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, TBL_LINE, 0);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (uint8_t r = 0; r < rows; r++) {
        lv_obj_t *row = lv_obj_create(grid);
        lv_obj_set_size(row, lv_pct(100), TBL_CELL_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);      /* 行底透明：格间露出表格线 */
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, TBL_LINE, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        for (uint8_t c = 0; c < cols; c++) {
            const uint8_t i = (uint8_t)(r * cols + c);

            /* 每格：卡片底、无描边无圆角（表格样式），一行里等分；标题靠左、数值靠右 */
            lv_obj_t *cell = lv_obj_create(row);
            lv_obj_set_height(cell, lv_pct(100));
            lv_obj_set_flex_grow(cell, 1);
            lv_obj_set_style_bg_color(cell, fw_theme_color_bg_card(), 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_set_style_pad_left(cell, 5, 0);
            lv_obj_set_style_pad_right(cell, 5, 0);
            lv_obj_set_scrollable(cell, false);
            lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);

            lv_obj_t *name = lv_label_create(cell);
            lv_label_set_text(name, (labels != NULL && labels[i] != NULL) ? labels[i] : "");
            lv_obj_set_style_text_font(name, fw_asset_font_cn(), 0);
            lv_obj_set_style_text_color(name, fw_theme_color_text_secondary(), 0);

            lv_obj_t *val = lv_label_create(cell);
            lv_label_set_text(val, "--");
            lv_obj_set_style_text_font(val, fw_asset_font_cn(), 0);
            lv_obj_set_style_text_color(val, fw_theme_color_text_primary(), 0);
        }
    }

    lvgl_port_unlock();
    return grid;
}

/* 找到某一格的数值标签（索引 = 行 × 列数 + 列；列数由第一行的格子数反推）。
 * 调用者必须已持 LVGL 锁。0 = 标题，1 = 数值。 */
static lv_obj_t *table_find_value(lv_obj_t *table, uint8_t index)
{
    if (table == NULL) return NULL;

    lv_obj_t *first = lv_obj_get_child(table, 0);
    const uint32_t cols = (first != NULL) ? lv_obj_get_child_count(first) : 0;
    if (cols == 0) return NULL;

    lv_obj_t *row = lv_obj_get_child(table, (int32_t)(index / cols));
    lv_obj_t *cell = (row != NULL) ? lv_obj_get_child(row, (int32_t)(index % cols)) : NULL;
    return (cell != NULL) ? lv_obj_get_child(cell, 1) : NULL;
}

esp_err_t fw_ui_table_value(lv_obj_t *table, uint8_t index, const char *value)
{
    lvgl_port_lock(0);

    lv_obj_t *val = table_find_value(table, index);
    if (val == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    lv_label_set_text(val, (value != NULL) ? value : "--");
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t fw_ui_table_value_scroll(lv_obj_t *table, uint8_t index, bool on)
{
    lvgl_port_lock(0);

    lv_obj_t *val = table_find_value(table, index);
    if (val == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_ARG;
    }

    if (on) {
        /* 定宽才滚得起来（占格子 55%）：短值照常显示，超长才横向滚动 */
        lv_obj_set_width(val, lv_pct(55));
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    } else {
        lv_obj_set_width(val, LV_SIZE_CONTENT);
        lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_WRAP);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

void fw_ui_format_size(char *buf, size_t len, uint64_t bytes)
{
    static const char *const unit[] = { "B", "KB", "MB", "GB" };
    double v = (double)bytes;
    size_t u = 0;

    while (v >= 1024.0 && u + 1 < sizeof(unit) / sizeof(unit[0])) {
        v /= 1024.0;
        u++;
    }

    if (u == 0) snprintf(buf, len, "%u B", (unsigned)bytes);
    else if (u == 1) snprintf(buf, len, "%.0f KB", v);
    else snprintf(buf, len, "%.1f %s", v, unit[u]);
}

/* ------------------------------- 浮层常用件 ------------------------------- */

lv_obj_t *fw_ui_action_btn(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text,
                           int32_t w, lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 44);
    if (w > 0) {
        lv_obj_set_width(btn, w);
    } else {
        lv_obj_set_flex_grow(btn, 1);
    }
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_column(btn, 6, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    if (icon != NULL) fw_ui_icon(btn, icon, fw_theme_color_accent());

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text != NULL ? text : "");
    lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);

    lvgl_port_unlock();
    return btn;
}

lv_obj_t *fw_ui_btn(lv_obj_t *parent, const char *text, int32_t w, bool primary,
                    lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, 30);
    lv_obj_set_style_bg_color(btn, primary ? fw_theme_color_accent()
                                           : fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, primary ? 0 : 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, text != NULL ? text : "");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, primary ? lv_color_white()
                                            : fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);

    lvgl_port_unlock();
    return btn;
}

lv_obj_t *fw_ui_textarea(lv_obj_t *parent, const char *placeholder)
{
    lvgl_port_lock(0);

    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    if (placeholder != NULL) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_height(ta, 36);
    lv_obj_set_style_bg_color(ta, fw_theme_color_bg_secondary(), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_left(ta, 8, 0);
    lv_obj_set_style_pad_right(ta, 8, 0);
    lv_obj_set_style_text_font(ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(ta, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_text_color(ta, fw_theme_color_text_disabled(),
                                LV_PART_TEXTAREA_PLACEHOLDER);

    lvgl_port_unlock();
    return ta;
}

/* ---------------------------------- 图标 ---------------------------------- */

lv_obj_t *fw_ui_icon(lv_obj_t *parent, const lv_image_dsc_t *icon, lv_color_t color)
{
    if (icon == NULL) return NULL;

    lvgl_port_lock(0);

    lv_obj_t *img = lv_image_create(parent != NULL ? parent : lv_layer_top());
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    lvgl_port_unlock();
    return img;
}

lv_obj_t *fw_ui_icon_btn(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text,
                         int32_t w, lv_event_cb_t cb, void *user)
{
    if (icon == NULL) return NULL;

    lvgl_port_lock(0);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, 28);
    if (w > 0) {
        lv_obj_set_width(btn, w);
    } else {
        lv_obj_set_flex_grow(btn, 1);
    }
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 4);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user);

    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, icon);
    lv_obj_set_style_image_recolor(img, fw_theme_color_accent(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);

    if (text == NULL || text[0] == '\0') {
        lv_obj_center(img);
    } else {
        lv_obj_align(img, LV_ALIGN_LEFT_MID, 7, 0);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, text);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
        if (w <= 0) lv_obj_set_width(lb, lv_pct(74));   /* 撑满型按钮：给图标留出位置 */
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 31, 0);
    }

    lvgl_port_unlock();
    return btn;
}

esp_err_t fw_ui_row_btn_value(lv_obj_t *btn, const char *value)
{
    if (btn == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);

    uint32_t n = lv_obj_get_child_count(btn);
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

esp_err_t fw_ui_list_item_value(lv_obj_t *item, const char *value)
{
    /* 列表项的右侧数值和有右侧数值位的行按钮标记方式一致（user_data != NULL），
     * 所以直接复用同一段查找 */
    return fw_ui_row_btn_value(item, value);
}

lv_obj_t *fw_ui_slider_row(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *label,
                           int32_t min, int32_t max, int32_t value, lv_event_cb_t cb, void *user)
{
    lvgl_port_lock(0);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_set_scrollable(row, false);
    /* 和行入口一样是"扁平行"：透明底 + 底部 1 px 分隔线 */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, fw_theme_color_divider(), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (icon != NULL) {
        lv_obj_t *ic = lv_image_create(row);
        lv_image_set_src(ic, icon);
        lv_obj_set_style_image_recolor(ic, fw_theme_color_accent(), 0);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, 0);
    }

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label != NULL ? label : "");
    lv_obj_set_style_text_font(l, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(l, fw_theme_color_text_primary(), 0);

    lv_obj_t *bar = lv_slider_create(row);
    lv_obj_set_height(bar, 16);
    lv_obj_set_flex_grow(bar, 1);
    lv_slider_set_range(bar, min, max);
    lv_slider_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, fw_theme_color_bg_secondary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, fw_theme_color_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, fw_theme_color_accent(), LV_PART_KNOB);
    lv_obj_set_style_border_width(bar, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(bar, fw_theme_color_border(), LV_PART_KNOB);
    if (cb != NULL) lv_obj_add_event_cb(bar, cb, LV_EVENT_VALUE_CHANGED, user);

    /* 右侧数值文本：先建好（空），由 fw_ui_slider_row_value() 更新；用 user_data 标记便于查找 */
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "");
    lv_obj_set_style_text_font(v, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(v, fw_theme_color_text_secondary(), 0);
    lv_obj_set_user_data(v, (void *)1);

    lvgl_port_unlock();
    return bar;
}

esp_err_t fw_ui_slider_row_value(lv_obj_t *slider, const char *text)
{
    if (slider == NULL) return ESP_ERR_INVALID_ARG;

    lvgl_port_lock(0);

    lv_obj_t *row = lv_obj_get_parent(slider);
    if (row == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
        if (lv_obj_get_user_data(child) != NULL) {
            lv_label_set_text(child, text != NULL ? text : "");
            lvgl_port_unlock();
            return ESP_OK;
        }
    }

    lvgl_port_unlock();
    return ESP_ERR_NOT_FOUND;
}

/* --------------------------------- 分段构建 --------------------------------- */

/*
 * 列表 / 网格几百项时不在一次 LVGL 周期里全建出来：每次建一小批，剩下的交给
 * 16 ms 定时器。这样 LVGL 任务不会被霸占几百毫秒，构建期间触摸与状态栏照常。
 *
 * 定时器在 LVGL 任务里跑，自删是 LVGL 支持的用法，
 * 所以建完直接在回调里删掉它。
 */
struct fw_ui_stage {
    lv_timer_t *timer;
    size_t total;
    size_t chunk;
    size_t idx;
    fw_ui_stage_build_cb_t build;
    fw_ui_stage_done_cb_t done;
    void *user;
};

static void stage_run(fw_ui_stage_t *st)
{
    if (st == NULL || st->build == NULL) return;

    size_t end = st->idx + st->chunk;
    if (end > st->total) end = st->total;

    lvgl_port_lock(0);
    for (; st->idx < end; st->idx++) {
        st->build(st->idx, st->user);
    }
    lvgl_port_unlock();

    if (st->idx >= st->total) {
        fw_ui_stage_stop(st);

        /* 先清掉回调再调 done：done 里可能又 start 一轮新的构建 */
        fw_ui_stage_done_cb_t done = st->done;
        st->build = NULL;
        st->done = NULL;
        if (done != NULL) done(st->user);
    }
}

static void stage_timer_cb(lv_timer_t *t)
{
    stage_run((fw_ui_stage_t *)lv_timer_get_user_data(t));
}

fw_ui_stage_t *fw_ui_stage_create(void)
{
    return calloc(1, sizeof(fw_ui_stage_t));
}

esp_err_t fw_ui_stage_start(fw_ui_stage_t *st, size_t total, size_t chunk,
                            fw_ui_stage_build_cb_t build, fw_ui_stage_done_cb_t done, void *user)
{
    if (build == NULL) return ESP_ERR_INVALID_ARG;

    /* 句柄没建起来（内存不足）：退化成一次性建完，界面不至于空着 */
    if (st == NULL) {
        ESP_LOGW(TAG, "stage alloc failed, building %u item(s) at once", (unsigned)total);
        lvgl_port_lock(0);
        for (size_t i = 0; i < total; i++) build(i, user);
        if (done != NULL) done(user);
        lvgl_port_unlock();
        return ESP_OK;
    }

    fw_ui_stage_stop(st);

    st->total = total;
    st->chunk = (chunk > 0) ? chunk : 1;
    st->idx = 0;
    st->build = build;
    st->done = done;
    st->user = user;

    if (total == 0) {
        lvgl_port_lock(0);
        if (done != NULL) done(user);
        lvgl_port_unlock();
        return ESP_OK;
    }

    lvgl_port_lock(0);
    st->timer = lv_timer_create(stage_timer_cb, FW_UI_STAGE_PERIOD_MS, st);
    lvgl_port_unlock();

    if (st->timer == NULL) {
        /* 定时器没建起来：放开分段，下面这一趟直接全建完 */
        ESP_LOGW(TAG, "stage timer create failed; building %u item(s) at once", (unsigned)total);
        st->chunk = total;
    }

    stage_run(st);      /* 第一批立刻建：小列表一趟就完事，没有分段开销 */
    return ESP_OK;
}

void fw_ui_stage_stop(fw_ui_stage_t *st)
{
    if (st == NULL || st->timer == NULL) return;

    lvgl_port_lock(0);
    lv_timer_delete(st->timer);
    lvgl_port_unlock();

    st->timer = NULL;
}

void fw_ui_stage_destroy(fw_ui_stage_t *st)
{
    if (st == NULL) return;

    fw_ui_stage_stop(st);
    free(st);
}
