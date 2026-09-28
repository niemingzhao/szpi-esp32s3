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
 */
esp_err_t fw_ui_dialog_close(lv_obj_t *dlg);

/**
 * @brief 弹出 Toast（duration_ms 为 0 时默认 3000 ms，自动消失）
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
 * @brief 创建网格容器（ROW_WRAP，每行 cols 个，item_w/item_h 用于计算宽度）
 */
lv_obj_t *fw_ui_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t item_w, lv_coord_t item_h);

#ifdef __cplusplus
}
#endif
