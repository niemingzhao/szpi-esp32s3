/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 存储（容量 / 目录 / 文件读写）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 存储类型
 */
typedef enum {
    SVC_STORAGE_TF_CARD,         /* /sdcard (FAT) */
    SVC_STORAGE_INTERNAL_FLASH,  /* /internal (SPIFFS) */
} svc_storage_type_t;

/**
 * @brief 存储容量
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
} svc_storage_info_t;

/**
 * @brief 初始化存储服务
 */
esp_err_t svc_storage_init(void);

/**
 * @brief 新建目录（已存在按成功处理）
 */
esp_err_t svc_storage_mkdir(const char *path);

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
esp_err_t svc_storage_get_info(svc_storage_type_t type, svc_storage_info_t *out);

/**
 * @brief 格式化（谨慎！会清空该存储的全部数据；界面上必须二次确认）
 */
esp_err_t svc_storage_format(svc_storage_type_t type);

/** 整块读取文件的上限（1 MB），防止误读大文件把内部 RAM 吃光 */
#define SVC_STORAGE_READ_MAX   (1024u * 1024u)

/**
 * @brief 整块读一个文件（成功时 *out_buf 用 malloc 分配，调用方负责 free）
 *
 * 适合文本、配置、小图片；超过 SVC_STORAGE_READ_MAX 直接失败。
 * 空文件返回长度为 0 的空缓冲（buf[0] = '\0'）。
 */
esp_err_t svc_storage_read(const char *path, void **out_buf, size_t *out_len);

/**
 * @brief 整块写一个文件（覆盖写；len 为 0 时创建空文件）
 */
esp_err_t svc_storage_write(const char *path, const void *data, size_t len);

/**
 * @brief 追加写一个文件（不存在时创建）
 *
 * 供脚本写日志这类"只往后加"的用途，不用先把整个文件读回来。
 */
esp_err_t svc_storage_append(const char *path, const void *data, size_t len);

/**
 * @brief 流式写的句柄（上传大文件用：整块写要把整个文件放进内存）
 */
typedef struct svc_storage_writer_s svc_storage_writer_t;

/**
 * @brief 开始流式写（覆盖写：先建 / 截断），成功后要配对调用 write_close
 */
esp_err_t svc_storage_write_open(const char *path, svc_storage_writer_t **out);

/**
 * @brief 追加一块数据
 *
 * 内部走和 svc_storage_write() 同一条路径（非内部 RAM 的来源先过一块内部 DMA 缓冲），
 * 所以 flash / PSRAM 里的数据都安全。
 */
esp_err_t svc_storage_write_chunk(svc_storage_writer_t *w, const void *data, size_t len);

/**
 * @brief 结束流式写（关文件并释放句柄）
 *
 * 上传中途失败时由调用方自己 svc_storage_remove() 删掉半截文件。
 */
esp_err_t svc_storage_write_close(svc_storage_writer_t *w);

/**
 * @brief 流式读的回调；返回非 ESP_OK 立即停止（例如 HTTP 客户端断开）
 */
typedef esp_err_t (*svc_storage_read_cb_t)(const void *data, size_t len, void *user);

/**
 * @brief 分块读整个文件并交给回调（大文件下发用；整块读有 1 MB 上限）
 *
 * @param[out] out_size 文件总字节数（可为 NULL，回调失败时也已填好）
 * @return 文件打不开返回 ESP_ERR_NOT_FOUND，否则返回回调的结果
 */
esp_err_t svc_storage_stream_read(const char *path, size_t *out_size,
                                  svc_storage_read_cb_t cb, void *user);

/**
 * @brief 删除文件
 */
esp_err_t svc_storage_remove(const char *path);

/**
 * @brief 删除文件或目录（目录连同内容一起删；不会删存储根目录）
 */
esp_err_t svc_storage_remove_tree(const char *path);

/**
 * @brief 重命名 / 移动（同一存储内是改目录项，瞬间完成）
 *
 * 目标已存在时返回 ESP_ERR_INVALID_STATE，由调用方换成不重名的目标；
 * 跨存储（TF ↔ 内置）rename 会失败，请改用 svc_storage_copy() + svc_storage_remove_tree()。
 */
esp_err_t svc_storage_rename(const char *from, const char *to);

/**
 * @brief 复制文件或目录（目录递归；分块拷贝，大文件也不会整块读进内存）
 *
 * 目标存在时直接覆盖其中的同名文件，调用方应先保证目标名不重复。
 */
esp_err_t svc_storage_copy(const char *from, const char *to);

/**
 * @brief 判断路径是否存在，可选返回大小（目录按 0 返回）
 */
esp_err_t svc_storage_exists(const char *path, size_t *out_size);

#ifdef __cplusplus
}
#endif
