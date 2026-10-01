/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Storage 实现（封装 periph_storage + 路径/目录辅助）
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "svc.storage";

#define SD_POLL_MS        1000    /* TF 卡在/不在 检测周期 */
#define SD_MOUNT_RETRY    10      /* 没卡时每 10 次轮询才尝试挂载一次，避免刷日志 */
#define SD_POLL_STACK     3072

/* TF 卡热插拔：板子没有卡检测引脚，只能轮询"卡是否还能访问" */
static void sd_poll_task(void *arg)
{
    (void)arg;

    bool present = periph_storage_is_mounted(PERIPH_STORAGE_TF_CARD);
    int retry = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SD_POLL_MS));

        if (!present) {
            if (++retry < SD_MOUNT_RETRY) continue;
            retry = 0;

            if (periph_storage_mount(PERIPH_STORAGE_TF_CARD) == ESP_OK) {
                present = true;
                ESP_LOGI(TAG, "TF card inserted");
                svc_event_bus_publish(SVC_EVENT_SD_MOUNTED, NULL, 0);
            }
            continue;
        }

        if (!periph_storage_tf_card_present()) {
            ESP_LOGW(TAG, "TF card removed");
            periph_storage_unmount(PERIPH_STORAGE_TF_CARD);
            present = false;
            svc_event_bus_publish(SVC_EVENT_SD_UNMOUNTED, NULL, 0);
        }
    }
}

esp_err_t svc_storage_init(void)
{
    if (xTaskCreatePinnedToCore(sd_poll_task, "sd_monitor_task", SD_POLL_STACK, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "sd poll task create failed");
    }

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_storage_get_path(periph_storage_type_t type, char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    const char *path = (type == PERIPH_STORAGE_TF_CARD) ? "/sdcard" : "/internal";
    strlcpy(buf, path, len);
    return ESP_OK;
}

esp_err_t svc_storage_app_dir(const char *app_name, char *buf, size_t len)
{
    if (app_name == NULL || buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    int n = snprintf(buf, len, "/internal/apps/%s", app_name);
    if (n < 0 || (size_t)n >= len) return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

esp_err_t svc_storage_mkdir(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (mkdir(path, 0777) == 0) return ESP_OK;
    return (errno == EEXIST) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_storage_rmdir(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    return (rmdir(path) == 0) ? ESP_OK : ESP_FAIL;
}

typedef struct {
    DIR *dir;
    char base[256];
    svc_storage_entry_t entry;
} svc_iter_ctx_t;

esp_err_t svc_storage_iter_start(const char *dir, svc_storage_iter_t *iter)
{
    if (dir == NULL || iter == NULL) return ESP_ERR_INVALID_ARG;

    svc_iter_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) return ESP_ERR_NO_MEM;

    ctx->dir = opendir(dir);
    if (ctx->dir == NULL) {
        free(ctx);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(ctx->base, dir, sizeof(ctx->base));
    *iter = ctx;
    return ESP_OK;
}

svc_storage_entry_t *svc_storage_iter_next(svc_storage_iter_t iter)
{
    svc_iter_ctx_t *ctx = (svc_iter_ctx_t *)iter;
    if (ctx == NULL || ctx->dir == NULL) return NULL;

    struct dirent *de;
    while ((de = readdir(ctx->dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        strlcpy(ctx->entry.name, de->d_name, sizeof(ctx->entry.name));
        ctx->entry.is_dir = (de->d_type == DT_DIR);
        ctx->entry.size = 0;

        if (!ctx->entry.is_dir) {
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", ctx->base, de->d_name);
            struct stat st;
            if (stat(full, &st) == 0) ctx->entry.size = (size_t)st.st_size;
        }
        return &ctx->entry;
    }
    return NULL;
}

void svc_storage_iter_end(svc_storage_iter_t iter)
{
    svc_iter_ctx_t *ctx = (svc_iter_ctx_t *)iter;
    if (ctx == NULL) return;
    if (ctx->dir) closedir(ctx->dir);
    free(ctx);
}

esp_err_t svc_storage_get_info(periph_storage_type_t type, periph_storage_info_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    return periph_storage_get_info(type, out);
}

esp_err_t svc_storage_format(periph_storage_type_t type)
{
    ESP_LOGW(TAG, "formatting storage %d", (int)type);
    return periph_storage_format(type);
}

esp_err_t svc_storage_read(const char *path, void **out_buf, size_t *out_len)
{
    if (path == NULL || out_buf == NULL || out_len == NULL) return ESP_ERR_INVALID_ARG;

    *out_buf = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL) return ESP_ERR_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || size > SVC_STORAGE_READ_MAX) {
        fclose(f);
        return (size == 0) ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }

    buf[size] = '\0';    /* 读文本时省事；二进制长度以 *out_len 为准 */
    *out_buf = buf;
    *out_len = (size_t)size;
    return ESP_OK;
}

esp_err_t svc_storage_write(const char *path, const void *data, size_t len)
{
    if (path == NULL || data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "wb");
    if (f == NULL) return ESP_FAIL;

    size_t n = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || n != len) return ESP_FAIL;

    ESP_LOGD(TAG, "wrote %u byte(s) to %s", (unsigned)len, path);
    return ESP_OK;
}

esp_err_t svc_storage_remove(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;

    return (remove(path) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t svc_storage_exists(const char *path, size_t *out_size)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (S_ISDIR(st.st_mode)) return ESP_ERR_INVALID_ARG;

    if (out_size != NULL) *out_size = (size_t)st.st_size;
    return ESP_OK;
}
