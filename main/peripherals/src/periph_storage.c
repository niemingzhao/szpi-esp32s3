/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 存储（TF 卡 SDMMC 1-bit / 内置 SPIFFS）
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
        esp_err_t err = esp_vfs_fat_sdcard_unmount(TF_MOUNT_POINT, s_tf_card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TF card unmount failed: %s", esp_err_to_name(err));
            return err;
        }
        s_tf_mounted = false;
        s_tf_card = NULL;
        ESP_LOGI(TAG, "TF card unmounted");
        return ESP_OK;
    }

    if (!s_flash_mounted) return ESP_OK;
    esp_err_t err = esp_vfs_spiffs_unregister(INTERNAL_PARTITION);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal SPIFFS unmount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_flash_mounted = false;
    ESP_LOGI(TAG, "internal SPIFFS unmounted");
    return ESP_OK;
}

bool periph_storage_is_mounted(periph_storage_type_t type)
{
    if (type == PERIPH_STORAGE_TF_CARD) return s_tf_mounted;
    return s_flash_mounted;
}

bool periph_storage_tf_card_present(void)
{
    if (!s_tf_mounted || s_tf_card == NULL) return false;

    /* 卡被拔出后总线访问会失败 */
    return (sdmmc_get_status(s_tf_card) == ESP_OK);
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

esp_err_t periph_storage_format(periph_storage_type_t type)
{
    if (type == PERIPH_STORAGE_TF_CARD) {
        if (!s_tf_mounted) return ESP_ERR_INVALID_STATE;

        /* esp_vfs_fat_sdcard_format() 内部就是 unmount → 格式化 → mount 回来；但它内部
         * mount 失败时会把 card 释放掉却仍返回 ESP_OK（s_tf_card 变悬空）。这里用挂载点
         * 还能不能查到容量来判断：查不到就当作已卸载，自己再挂一次。 */
        esp_err_t err = esp_vfs_fat_sdcard_format(TF_MOUNT_POINT, s_tf_card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TF card format failed: %s", esp_err_to_name(err));
            return err;
        }

        uint64_t total = 0, free_bytes = 0;
        if (esp_vfs_fat_info(TF_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
            ESP_LOGW(TAG, "TF card not remounted after format, remounting");
            /* card 已被释放，不能再拿它去 unmount */
            s_tf_mounted = false;
            s_tf_card = NULL;
            err = periph_storage_mount(PERIPH_STORAGE_TF_CARD);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "TF card remount failed: %s", esp_err_to_name(err));
                return err;
            }
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
