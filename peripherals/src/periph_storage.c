/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Storage
 *
 *   - TF 卡：SDMMC 1-bit → /sdcard（FAT，无卡时不报错）
 *   - 内置 Flash：storage 分区 → /internal（SPIFFS）
 *
 * 说明：分区表中 storage 分区 subtype 为 spiffs，故内置文件系统使用 SPIFFS。
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "periph.storage";

#define TF_MOUNT_POINT       "/sdcard"
#define INTERNAL_MOUNT_POINT "/internal"
#define INTERNAL_PARTITION   "storage"

#define SD_PIN_CLK   GPIO_NUM_47
#define SD_PIN_CMD   GPIO_NUM_48
#define SD_PIN_D0    GPIO_NUM_21

static bool s_tf_mounted = false;
static bool s_flash_mounted = false;
static sdmmc_card_t *s_tf_card = NULL;

esp_err_t periph_storage_init(void)
{
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t periph_storage_mount(periph_storage_type_t type)
{
    if (type == PERIPH_STORAGE_TF_CARD) {
        if (s_tf_mounted) return ESP_OK;

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = 1;  /* 1-bit SD 模式 */
        slot_config.clk = SD_PIN_CLK;
        slot_config.cmd = SD_PIN_CMD;
        slot_config.d0  = SD_PIN_D0;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_vfs_fat_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 4096,
        };

        esp_err_t err = esp_vfs_fat_sdmmc_mount(TF_MOUNT_POINT, &host, &slot_config,
                                                 &mount_cfg, &s_tf_card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TF card mount failed: %s", esp_err_to_name(err));
            return err;
        }

        s_tf_mounted = true;
        ESP_LOGI(TAG, "TF card mounted at %s", TF_MOUNT_POINT);
        return ESP_OK;
    }

    if (s_flash_mounted) return ESP_OK;

    const esp_vfs_spiffs_conf_t spiffs_cfg = {
        .base_path = INTERNAL_MOUNT_POINT,
        .partition_label = INTERNAL_PARTITION,
        .max_files = 5,
        .format_if_mount_failed = true,  /* 首次挂载失败时格式化（全新设备开箱即用） */
    };
    esp_err_t err = esp_vfs_spiffs_register(&spiffs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(INTERNAL_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "internal SPIFFS mounted at %s (%u/%u KB used)",
                 INTERNAL_MOUNT_POINT, (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    s_flash_mounted = true;
    return ESP_OK;
}

esp_err_t periph_storage_unmount(periph_storage_type_t type)
{
    if (type == PERIPH_STORAGE_TF_CARD) {
        if (!s_tf_mounted) return ESP_OK;
        esp_vfs_fat_sdcard_unmount(TF_MOUNT_POINT, s_tf_card);
        s_tf_mounted = false;
        s_tf_card = NULL;
        ESP_LOGI(TAG, "TF card unmounted");
        return ESP_OK;
    }

    if (!s_flash_mounted) return ESP_OK;
    esp_vfs_spiffs_unregister(INTERNAL_PARTITION);
    s_flash_mounted = false;
    ESP_LOGI(TAG, "internal SPIFFS unmounted");
    return ESP_OK;
}

bool periph_storage_is_mounted(periph_storage_type_t type)
{
    if (type == PERIPH_STORAGE_TF_CARD) return s_tf_mounted;
    return s_flash_mounted;
}

esp_err_t periph_storage_get_info(periph_storage_type_t type, periph_storage_info_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    if (type == PERIPH_STORAGE_TF_CARD) {
        if (!s_tf_mounted) return ESP_ERR_INVALID_STATE;
        return esp_vfs_fat_info(TF_MOUNT_POINT, &out->total_bytes, &out->free_bytes);
    }

    if (!s_flash_mounted) return ESP_ERR_INVALID_STATE;
    size_t total = 0, used = 0;
    esp_err_t err = esp_spiffs_info(INTERNAL_PARTITION, &total, &used);
    if (err != ESP_OK) return err;
    out->total_bytes = total;
    out->free_bytes = total - used;
    return ESP_OK;
}

/* 拼接挂载点 + 相对路径 */
static esp_err_t build_full_path(periph_storage_type_t type, const char *path,
                                 char *out, size_t out_len)
{
    if (path == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    if (!periph_storage_is_mounted(type)) return ESP_ERR_INVALID_STATE;

    const char *root = (type == PERIPH_STORAGE_TF_CARD) ? TF_MOUNT_POINT : INTERNAL_MOUNT_POINT;
    int n = snprintf(out, out_len, "%s%s%s", root, (path[0] == '/') ? "" : "/", path);
    if (n < 0 || (size_t)n >= out_len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t periph_storage_file_exists(periph_storage_type_t type, const char *path)
{
    char full[256];
    esp_err_t err = build_full_path(type, path, full, sizeof(full));
    if (err != ESP_OK) return err;

    struct stat st;
    return (stat(full, &st) == 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t periph_storage_file_size(periph_storage_type_t type, const char *path, size_t *size)
{
    if (size == NULL) return ESP_ERR_INVALID_ARG;

    char full[256];
    esp_err_t err = build_full_path(type, path, full, sizeof(full));
    if (err != ESP_OK) return err;

    struct stat st;
    if (stat(full, &st) != 0) return ESP_ERR_NOT_FOUND;
    *size = (size_t)st.st_size;
    return ESP_OK;
}

esp_err_t periph_storage_file_delete(periph_storage_type_t type, const char *path)
{
    char full[256];
    esp_err_t err = build_full_path(type, path, full, sizeof(full));
    if (err != ESP_OK) return err;

    return (remove(full) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t periph_storage_file_rename(periph_storage_type_t type, const char *from, const char *to)
{
    char full_from[256];
    char full_to[256];
    esp_err_t err = build_full_path(type, from, full_from, sizeof(full_from));
    if (err != ESP_OK) return err;
    err = build_full_path(type, to, full_to, sizeof(full_to));
    if (err != ESP_OK) return err;

    return (rename(full_from, full_to) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t periph_storage_format(periph_storage_type_t type)
{
    if (type == PERIPH_STORAGE_TF_CARD) {
        if (!s_tf_mounted) return ESP_ERR_INVALID_STATE;
        esp_err_t err = esp_vfs_fat_sdcard_format(TF_MOUNT_POINT, s_tf_card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TF card format failed: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "TF card formatted");
        return ESP_OK;
    }

    if (s_flash_mounted) {
        esp_vfs_spiffs_unregister(INTERNAL_PARTITION);
        s_flash_mounted = false;
    }
    esp_err_t err = esp_spiffs_format(INTERNAL_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal SPIFFS format failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "internal SPIFFS formatted");
    return periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH);
}
