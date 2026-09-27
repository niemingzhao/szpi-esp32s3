/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Script（Lua 5.5 运行时 + 脚本管理 + 能力绑定）
 *
 * 能力清单（模块名 / 函数 / 数量上限）见实现文件头部的模块列表；
 * 板子上随固件释放的「脚本接口参考.txt」是同一份内容的可查版本。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 脚本目录（TF 卡，官方推荐目录；扫描范围不限于它） */
#define FW_SCRIPT_DIR           "/sdcard/scripts"
/** 单个脚本的显示名 / 说明长度上限 */
#define FW_SCRIPT_NAME_MAX      64
#define FW_SCRIPT_DESC_MAX      96
/** 完整路径长度上限（脚本可放在任意目录，路径会带目录层级） */
#define FW_SCRIPT_PATH_MAX      224
/** 脚本错误描述长度上限（fw_script_last_error()） */
#define FW_SCRIPT_ERR_MAX       128

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
 * @brief 扫描脚本（TF 卡与内置存储整盘，不限于 FW_SCRIPT_DIR）
 *
 * 递归遍历两个存储的目录树，收录所有 .lua（扩展名不分大小写）：FW_SCRIPT_DIR
 * 只是内置示例的落地目录，用户把脚本放别处一样能扫到。名称 / 说明取脚本头部的
 * -- @name / -- @desc，没有就用文件名。
 *
 * 目录层级与路径长度都有上限，超出的文件跳过并记一条告警（避免出现半截路径）。
 *
 * @param[out] out   结果数组
 * @param[in]  max   数组容量（满了就不再收，靠调用方扩容重扫）
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

/**
 * @brief 脚本页面现在是不是活动屏
 *
 * 返回键据此判断：是就结束脚本（fw_script_stop() 会把屏还给启动脚本的那个 App），
 * 不是就正常退 App。脚本没有 ui.page()（无界面脚本）时恒为 false。
 */
bool fw_script_owns_screen(void);

/**
 * @brief 最近一次脚本错误的描述（没有则 NULL）
 *
 * 脚本加载或运行出错时写入（形如 /sdcard/scripts/demo.lua:12: attempt to …），
 * 成功启动与正常停止后清空；供脚本管理说明"为什么没跑起来"。
 */
const char *fw_script_last_error(void);

#ifdef __cplusplus
}
#endif
