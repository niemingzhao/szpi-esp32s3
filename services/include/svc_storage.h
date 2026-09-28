/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Storage
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "periph_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化存储服务
 */
esp_err_t svc_storage_init(void);

/**
 * @brief 获取文件系统根路径（"/sdcard" 或 "/internal"）
 */
esp_err_t svc_storage_get_path(periph_storage_type_t type, char *buf, size_t len);

/**
 * @brief 获取 App 私有目录（/internal/apps/<app_name>）
 */
esp_err_t svc_storage_app_dir(const char *app_name, char *buf, size_t len);

/**
 * @brief 目录操作
 */
esp_err_t svc_storage_mkdir(const char *path);
esp_err_t svc_storage_rmdir(const char *path);

/**
 * @brief 目录项
 */
typedef struct {
    char name[256];
    bool is_dir;
    size_t size;
} svc_storage_entry_t;

/**
 * @brief 目录迭代器
 */
typedef void *svc_storage_iter_t;
esp_err_t svc_storage_iter_start(const char *dir, svc_storage_iter_t *iter);
svc_storage_entry_t *svc_storage_iter_next(svc_storage_iter_t iter);
void svc_storage_iter_end(svc_storage_iter_t iter);

#ifdef __cplusplus
}
#endif
