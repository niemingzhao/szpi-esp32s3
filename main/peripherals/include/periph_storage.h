/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 存储（TF 卡 FAT + 内置 SPIFFS）
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 存储类型
 */
typedef enum {
    PERIPH_STORAGE_TF_CARD,         // /sdcard (FAT)
    PERIPH_STORAGE_INTERNAL_FLASH,  // /internal (SPIFFS)
} periph_storage_type_t;

/**
 * @brief 存储信息
 */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
} periph_storage_info_t;

/**
 * @brief 初始化存储
 */
esp_err_t periph_storage_init(void);

/**
 * @brief 挂载存储
 */
esp_err_t periph_storage_mount(periph_storage_type_t type);

/**
 * @brief 卸载存储
 */
esp_err_t periph_storage_unmount(periph_storage_type_t type);

/**
 * @brief 查询挂载状态
 */
bool periph_storage_is_mounted(periph_storage_type_t type);

/**
 * @brief TF 卡是否仍可访问（热插拔检测：卡被拔出后会返回 false）
 *
 * 未挂载或卡已拔出都返回 false；调用它不会改变挂载状态。
 */
bool periph_storage_tf_card_present(void);

/**
 * @brief 查询存储信息
 */
esp_err_t periph_storage_get_info(periph_storage_type_t type, periph_storage_info_t *out);

/**
 * @brief 格式化（谨慎！会清空数据）
 */
esp_err_t periph_storage_format(periph_storage_type_t type);

#ifdef __cplusplus
}
#endif
