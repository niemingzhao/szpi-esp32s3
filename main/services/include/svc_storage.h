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

/**
 * @brief 查询存储容量（总容量 / 剩余）
 */
esp_err_t svc_storage_get_info(periph_storage_type_t type, periph_storage_info_t *out);

/**
 * @brief 格式化（谨慎！会清空该存储的全部数据；界面上必须二次确认）
 */
esp_err_t svc_storage_format(periph_storage_type_t type);

/** 整块读取文件的上限（1 MB），防止误读大文件把内部 RAM 吃光 */
#define SVC_STORAGE_READ_MAX   (1024u * 1024u)

/**
 * @brief 整块读一个文件（成功时 *out_buf 用 malloc 分配，调用方负责 free）
 *
 * 适合文本、配置、小图片；超过 SVC_STORAGE_READ_MAX 直接失败。
 */
esp_err_t svc_storage_read(const char *path, void **out_buf, size_t *out_len);

/**
 * @brief 整块写一个文件（覆盖写）
 */
esp_err_t svc_storage_write(const char *path, const void *data, size_t len);

/**
 * @brief 删除文件
 */
esp_err_t svc_storage_remove(const char *path);

/**
 * @brief 判断文件是否存在，可选返回大小
 */
esp_err_t svc_storage_exists(const char *path, size_t *out_size);

#ifdef __cplusplus
}
#endif
