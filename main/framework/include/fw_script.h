/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Script（Lua 5.5 运行时 + 脚本管理 + 能力绑定）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 脚本目录（TF 卡） */
#define FW_SCRIPT_DIR           "/sdcard/scripts"
/** 单个脚本的显示名 / 说明长度上限 */
#define FW_SCRIPT_NAME_MAX      64
#define FW_SCRIPT_DESC_MAX      96
/** 完整路径长度上限 */
#define FW_SCRIPT_PATH_MAX      160

/** 脚本元信息（列表用） */
typedef struct {
    char path[FW_SCRIPT_PATH_MAX];
    char name[FW_SCRIPT_NAME_MAX];
    char desc[FW_SCRIPT_DESC_MAX];
} fw_script_info_t;

/**
 * @brief 初始化脚本子系统（建 Lua 命令队列与 script_task）
 */
esp_err_t fw_script_init(void);

/**
 * @brief 扫描脚本目录
 *
 * @param[out] out   结果数组
 * @param[in]  max   数组容量
 * @param[out] count 实际条数
 */
esp_err_t fw_script_scan(fw_script_info_t *out, size_t max, size_t *count);

/**
 * @brief 运行脚本（异步：投递给 script_task，成功后发布 SVC_EVENT_SCRIPT_STARTED）
 *
 * 同一时刻只运行一个前台脚本；已在运行时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t fw_script_run(const char *path);

/**
 * @brief 停止当前脚本（释放界面对象 / 定时器 / 订阅，发布 SVC_EVENT_SCRIPT_STOPPED）
 */
esp_err_t fw_script_stop(void);

/**
 * @brief 是否有脚本在运行
 */
bool fw_script_is_running(void);

/**
 * @brief 当前运行的脚本显示名（无则 NULL）
 */
const char *fw_script_current(void);

#ifdef __cplusplus
}
#endif
