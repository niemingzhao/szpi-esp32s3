/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - App Manager
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 桌面 App 名称（fw_app_mgr 以它作为返回栈的栈底） */
#define FW_APP_HOME_NAME   "Home"
/** 启动参数最大长度（含结尾 0） */
#define FW_APP_ARGS_MAX    128

/**
 * @brief App 描述符
 *
 * on_create 返回该 App 的根屏对象（lv_obj_create(NULL) 创建的 screen），
 * 它同时作为 on_start / on_pause / on_resume / on_destroy / on_back 的 ctx 传入。
 */
typedef struct {
    const char *name;              /* 内部标识（英文，启动 / 注册用） */
    const char *title;             /* 界面显示名（中文），桌面网格使用 */
    const lv_image_dsc_t *icon_64; /* 桌面图标，可为 NULL */
    const char *symbol;            /* LVGL 内置符号，icon_64 为 NULL 时使用 */
    void *(*on_create)(void);      /* 返回根屏对象 */
    void (*on_start)(void *ctx);
    void (*on_pause)(void *ctx);
    void (*on_resume)(void *ctx);
    void (*on_destroy)(void *ctx);
    /**
     * @brief 处理"返回"（状态栏返回键 / BOOT 单击）
     *
     * 返回 true 表示已在 App 内处理（例如回到上一级页面），fw_app_mgr 不再返回上一级；
     * 返回 false 或未实现则退出到上一级（通常是桌面）。可为 NULL。
     */
    bool (*on_back)(void *ctx);
} fw_app_desc_t;

/**
 * @brief 初始化 App 管理器（清空注册表与返回栈）
 */
esp_err_t fw_app_mgr_init(void);

/**
 * @brief 注册 App（名称重复返回 ESP_ERR_INVALID_ARG）
 */
esp_err_t fw_app_mgr_register(const fw_app_desc_t *desc);

/**
 * @brief 启动 / 切到某个 App
 *
 * 首次启动走 on_create + on_start；已在后台则走 on_resume；已在前台则忽略。
 * 会先暂停当前前台 App。内部自行加 LVGL 锁，可在任意任务中调用。
 */
esp_err_t fw_app_mgr_launch(const char *name);

/**
 * @brief 启动 App 并传入参数（App 内用 fw_app_mgr_get_args() 读取）
 */
esp_err_t fw_app_mgr_launch_with_args(const char *name, const char *args);

/**
 * @brief 通过 URI 启动 App，形如 "szpi://Music?song=1" 或 "Music?song=1"
 *
 * 用于 App 间通信（APP-004）与启动参数（APP-007）。
 */
esp_err_t fw_app_mgr_launch_uri(const char *uri);

/**
 * @brief 当前 App 的启动参数（没有参数时返回空字符串）
 */
const char *fw_app_mgr_get_args(void);

/**
 * @brief 返回上一级（暂停当前 App，恢复并显示栈中上一个）
 *
 * 已在栈底（Home）时为无操作。
 */
esp_err_t fw_app_mgr_back(void);

/**
 * @brief 回到 Home（暂停所有前台 App）
 */
esp_err_t fw_app_mgr_back_to_home(void);

/**
 * @brief 关闭并销毁一个非前台 App（释放其 LVGL 资源）
 */
esp_err_t fw_app_mgr_close(const char *name);

/**
 * @brief 当前前台 App 名（无则 NULL）
 */
const char *fw_app_mgr_current(void);

/**
 * @brief 当前前台是否为 Home
 */
bool fw_app_mgr_is_home(void);

/**
 * @brief 已注册 App 列表（供桌面网格使用）
 *
 * @param out 输出数组
 * @param max out 容量
 * @return 实际写入的条目数
 */
size_t fw_app_mgr_list(const fw_app_desc_t **out, size_t max);

/**
 * @brief 已注册 App 数量
 */
size_t fw_app_mgr_count(void);

/**
 * @brief 换主题后重建所有已创建 App 的界面，并保持当前前台 App
 */
esp_err_t fw_app_mgr_rebuild_all(void);

#ifdef __cplusplus
}
#endif
