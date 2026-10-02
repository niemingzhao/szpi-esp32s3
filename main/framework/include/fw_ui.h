/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - UI 通用组件
 *
 * 进度条 / 对话框 / Toast / 列表 / 网格，供各 App 与 Framework 内部复用。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "fw_ui_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 对话框按钮（位掩码，可组合）
 */
typedef enum {
    FW_DIALOG_BTN_NONE     = 0,
    FW_DIALOG_BTN_OK       = 1 << 0,
    FW_DIALOG_BTN_CANCEL   = 1 << 1,
    FW_DIALOG_BTN_YES      = 1 << 2,
    FW_DIALOG_BTN_NO       = 1 << 3,
    FW_DIALOG_BTN_SHUTDOWN = 1 << 4,
    FW_DIALOG_BTN_REBOOT   = 1 << 5,
} fw_dialog_btn_t;

/**
 * @brief 对话框按钮回调（在 LVGL 任务中执行）
 */
typedef void (*fw_dialog_cb_t)(fw_dialog_btn_t btn, void *user);

/**
 * @brief 初始化通用组件
 */
esp_err_t fw_ui_init(void);

/**
 * @brief 创建进度条（父对象为 NULL 时挂到 lv_layer_top()）
 */
lv_obj_t *fw_ui_progress_bar(lv_obj_t *parent, const char *title);

/**
 * @brief 设置进度 0-100
 */
esp_err_t fw_ui_progress_set(lv_obj_t *bar, uint8_t percent);

/**
 * @brief 创建模态对话框（同一时刻仅允许一个）
 *
 * @param parent 父对象，NULL 表示 lv_layer_top()
 * @param title  标题，可为 NULL
 * @param msg    正文，可为 NULL
 * @param buttons 按钮位掩码
 * @param cb     按钮点击回调，可为 NULL
 * @param user   回调用户参数
 * @return 遮罩对象（即对话框句柄），失败返回 NULL
 */
lv_obj_t *fw_ui_dialog(lv_obj_t *parent, const char *title, const char *msg,
                       fw_dialog_btn_t buttons, fw_dialog_cb_t cb, void *user);

/**
 * @brief 关闭对话框（dlg 为 NULL 时关闭当前对话框）
 *
 * 删除是异步的（可能在对话框自己的按钮回调里被调用）：关闭后要等删除生效
 * （下一帧）才能再开新对话框，期间 `fw_ui_dialog()` 会返回 NULL 并告警。
 */
esp_err_t fw_ui_dialog_close(lv_obj_t *dlg);

/**
 * @brief 弹出 Toast（duration_ms 为 0 时默认 3000 ms，自动消失）
 *
 * 同一时刻只保留一个：新 Toast 会顶掉上一个（不会叠在一起）。
 */
lv_obj_t *fw_ui_toast(const char *msg, uint32_t duration_ms);

/**
 * @brief 创建列表容器（可滚动），title 可为 NULL
 */
lv_obj_t *fw_ui_list(lv_obj_t *parent, const char *title);

/**
 * @brief 向列表追加一项
 */
lv_obj_t *fw_ui_list_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *user);

/**
 * @brief 向列表追加一项（右侧带数值，如信号强度 / 大小）
 */
lv_obj_t *fw_ui_list_add_value(lv_obj_t *list, const char *text, const char *value,
                               lv_event_cb_t cb, void *user);

/**
 * @brief 向列表追加一项（左侧图标 + 文本 + 右侧数值）
 */
lv_obj_t *fw_ui_list_add_icon(lv_obj_t *list, const lv_image_dsc_t *icon, const char *text,
                              const char *value, lv_event_cb_t cb, void *user);

/**
 * @brief 列表里的提示行（"正在扫描… / 未发现设备"这类）：无底色、无描边，次要色小字
 *
 * 提示不是可点的列表项，别用 fw_ui_list_add() 做成一个带描边的空壳。
 */
lv_obj_t *fw_ui_list_hint(lv_obj_t *list, const char *text);

/**
 * @brief 改列表项右侧数值的颜色（如按信号强度分色：强=success、弱=warning）
 */
esp_err_t fw_ui_list_value_color(lv_obj_t *item, lv_color_t color);

/**
 * @brief 把列表项标成"当前项"（主色描边 + 主色文字）
 */
esp_err_t fw_ui_list_mark(lv_obj_t *item, bool on);

/**
 * @brief 创建网格容器（ROW_WRAP，每行 cols 个）
 *
 * item_w 用于计算容器宽度（列间距 8、两侧内边距 6）；item_h 不参与排版，
 * 卡片尺寸由调用方自己设置。
 */
lv_obj_t *fw_ui_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t item_w, lv_coord_t item_h);

/**
 * @brief 创建 App 页面并返回根屏
 *
 * 根屏：bg_primary、无滚动、无内边距；内容容器从状态栏下方开始（320 × (240-28)，
 * 内边距 12、行距 8、纵向 flex），通过 content 返回（可为 NULL）。
 * on_create 直接返回本函数的根屏即可。
 */
lv_obj_t *fw_ui_page(lv_obj_t **content);

/**
 * @brief 创建整行入口按钮（高 50，卡片底色 + 1 px 描边；左侧图标 + 文本，右侧数值位）
 *
 * 数值用 fw_ui_row_btn_value() 更新。
 */
lv_obj_t *fw_ui_row_btn(lv_obj_t *parent, const char *symbol, const char *text,
                        lv_event_cb_t cb, void *user);

/**
 * @brief 更新整行按钮右侧的数值文本（按钮不是 fw_ui_row_btn 创建的会返回 ESP_ERR_NOT_FOUND）
 */
esp_err_t fw_ui_row_btn_value(lv_obj_t *btn, const char *value);

/**
 * @brief 创建"图标 + 标签 + 滑块 + 数值"一行（高 48，设置列表风格）
 *
 * 透明底 + 底部 1 px 分隔线（不做卡片，避免整页一叠卡片）；icon 用 icon_ui_*。
 * 右侧数值留空，用 fw_ui_slider_row_value() 更新；返回滑块供读取数值。
 */
lv_obj_t *fw_ui_slider_row(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *label,
                           int32_t min, int32_t max, int32_t value, lv_event_cb_t cb, void *user);

/**
 * @brief 更新"图标 + 标签 + 滑块"一行右侧的数值文本（slider 传 fw_ui_slider_row 的返回值）
 */
esp_err_t fw_ui_slider_row_value(lv_obj_t *slider, const char *text);

/**
 * @brief 内容卡片（圆角 12、卡片底 + 1 px 描边、内边距 10，高 h）
 *
 * 只给"内容"用（列表容器、信息块），普通设置项用 fw_ui_row_btn* 的扁平行。
 */
lv_obj_t *fw_ui_card(lv_obj_t *parent, lv_coord_t h);

/**
 * @brief 主信息卡：同 fw_ui_card，但用 2 px 主色描边突出（天气实况 / 当前网络这类页面主角）
 */
lv_obj_t *fw_ui_hero_card(lv_obj_t *parent, lv_coord_t h);

/**
 * @brief 设置项分组卡片：一整张卡片包住若干 fw_ui_row_btn* / fw_ui_slider_row
 *
 * 卡内行是"扁平行"（透明底 + 底部 1 px 分隔线），所以一页设置只有 1~2 张卡、
 * 卡内是列表 —— 既不像一叠卡片，也不会散成下划线列表。
 */
lv_obj_t *fw_ui_group(lv_obj_t *parent);

/**
 * @brief 分组间隔（透明占位，高 h）：设置项按功能分组时用
 */
lv_obj_t *fw_ui_gap(lv_obj_t *parent, lv_coord_t h);

/**
 * @brief 浮层里的按钮（高 30、圆角 8）
 *
 * primary = true 用主色底 + 白字（"连接 / 确定"这类主动作），false 用卡片底 + 描边 + 主色文字。
 * 浮层里的动作按钮都用它，保证两个浮层样式一致。
 */
lv_obj_t *fw_ui_btn(lv_obj_t *parent, const char *text, lv_coord_t w, bool primary,
                    lv_event_cb_t cb, void *user);

/**
 * @brief 浮层里的单行输入框（高 36、圆角 8、卡片底 + 描边、占位文字置灰）
 */
lv_obj_t *fw_ui_textarea(lv_obj_t *parent, const char *placeholder);

/**
 * @brief 创建 20×20 图标对象（用 icon_ui_* 资源，按 color 染色）
 *
 * 父对象为 NULL 时挂到 lv_layer_top()。调用方自己 align。
 */
lv_obj_t *fw_ui_icon(lv_obj_t *parent, const lv_image_dsc_t *icon, lv_color_t color);

/**
 * @brief 创建头部动作按钮（高 28，卡片底 + 描边），图标居中；text 非空时图标在左、文字在右
 *
 * @param w > 0 固定宽度，否则撑满剩余宽度（flex grow）
 */
lv_obj_t *fw_ui_icon_btn(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text,
                         lv_coord_t w, lv_event_cb_t cb, void *user);

/**
 * @brief 创建整行入口按钮（图标版）：高 50、卡片底 + 1 px 描边；左侧 20 px 图标（强调色）
 *
 * 数值用 fw_ui_row_btn_value() 更新。
 */
lv_obj_t *fw_ui_row_btn_img(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text,
                            lv_event_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
