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
 * @brief 创建"标签 + 滑块 + 数值"一行（高 30，滑块 16 高）；返回滑块供读取数值
 *
 * 右侧数值留空，用 fw_ui_slider_row_value() 更新；不需要数值时留空即可。
 */
lv_obj_t *fw_ui_slider_row(lv_obj_t *parent, const char *label, int32_t min, int32_t max,
                           int32_t value, lv_event_cb_t cb, void *user);

/**
 * @brief 更新"标签 + 滑块"一行右侧的数值文本（slider 传 fw_ui_slider_row 的返回值）
 */
esp_err_t fw_ui_slider_row_value(lv_obj_t *slider, const char *text);

#ifdef __cplusplus
}
#endif
