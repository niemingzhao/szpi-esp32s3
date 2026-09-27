/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 存储实现（封装 periph_storage，提供容量查询、目录迭代与整块读写）
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "esp_heap_caps.h"      /* heap_caps_malloc */
#include "esp_memory_utils.h"   /* esp_ptr_dma_capable */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>     /* rmdir */

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

static periph_storage_type_t to_periph(svc_storage_type_t type)
{
    return (type == SVC_STORAGE_TF_CARD) ? PERIPH_STORAGE_TF_CARD : PERIPH_STORAGE_INTERNAL_FLASH;
}

esp_err_t svc_storage_mkdir(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (mkdir(path, 0777) == 0) return ESP_OK;
    return (errno == EEXIST) ? ESP_OK : ESP_FAIL;
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

esp_err_t svc_storage_get_info(svc_storage_type_t type, svc_storage_info_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    periph_storage_info_t info;
    esp_err_t err = periph_storage_get_info(to_periph(type), &info);
    if (err != ESP_OK) return err;

    out->total_bytes = info.total_bytes;
    out->free_bytes = info.free_bytes;
    return ESP_OK;
}

esp_err_t svc_storage_format(svc_storage_type_t type)
{
    ESP_LOGW(TAG, "formatting storage %d", (int)type);
    return periph_storage_format(to_periph(type));
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
    if (size < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if (size > SVC_STORAGE_READ_MAX) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
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

/* 写文件：源数据可能在 flash 里（比如内置示例脚本的 rom const 字面量）。
 * FATFS 遇到"扇区对齐、并且一次写满整个扇区"的写入时会把调用者的缓冲直接交给
 * 底层（ff.c 的 f_write → disk_write → sdmmc_write_sectors），而 ESP32-S3 的 SDMMC
 * DMA 读不到映射在 flash 里的地址（SDMMC 只区分内部 RAM 和 PSRAM，不认 flash）。
 * 这类来源写进卡里就是一片 0，文件长度却完全正确（fwrite 照样返回全量字节），
 * 表现为"大小正常、打开却是空的"：编辑器显示空白，Lua 报 line 1: unexpected
 * symbol（首字节是 NUL）。所以只有内部 RAM 的缓冲直接写，其余（flash / PSRAM）
 * 一律先拷进一块内部 DMA 缓冲 —— S3 上 flash 与 PSRAM 共用同一段映射地址，指针上
 * 分不出差别，就一起拷，代价只是一次 memcpy。 */
#define STORAGE_WRITE_CHUNK   1024

static esp_err_t storage_write_all(FILE *f, const void *data, size_t len)
{
    if (len == 0) return ESP_OK;
    if (data == NULL) return ESP_ERR_INVALID_ARG;

    if (esp_ptr_dma_capable(data)) {            /* 内部 RAM：DMA 直接可用 */
        if (fwrite(data, 1, len, f) == len) return ESP_OK;
        ESP_LOGE(TAG, "write failed (direct), len=%u errno=%d", (unsigned)len, errno);
        return ESP_FAIL;
    }

    uint8_t *tmp = heap_caps_malloc(STORAGE_WRITE_CHUNK, MALLOC_CAP_DMA);
    if (tmp == NULL) {
        ESP_LOGE(TAG, "no internal DMA bounce buffer for len=%u", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    esp_err_t err = ESP_OK;

    while (left > 0) {
        const size_t n = (left < STORAGE_WRITE_CHUNK) ? left : STORAGE_WRITE_CHUNK;
        memcpy(tmp, p, n);
        if (fwrite(tmp, 1, n, f) != n) {
            ESP_LOGE(TAG, "write failed at %u/%u byte(s), errno=%d",
                     (unsigned)(len - left), (unsigned)len, errno);
            err = ESP_FAIL;
            break;
        }
        p += n;
        left -= n;
    }

    heap_caps_free(tmp);
    return err;
}

esp_err_t svc_storage_write(const char *path, const void *data, size_t len)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (data == NULL && len > 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "wb");
    if (f == NULL) return ESP_FAIL;

    const esp_err_t err = storage_write_all(f, data, len);
    if (fclose(f) != 0 || err != ESP_OK) return ESP_FAIL;

    ESP_LOGD(TAG, "wrote %u byte(s) to %s", (unsigned)len, path);
    return ESP_OK;
}

esp_err_t svc_storage_append(const char *path, const void *data, size_t len)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (data == NULL && len > 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "ab");            /* 不存在时创建 */
    if (f == NULL) return ESP_FAIL;

    const esp_err_t err = storage_write_all(f, data, len);
    if (fclose(f) != 0 || err != ESP_OK) return ESP_FAIL;

    ESP_LOGD(TAG, "appended %u byte(s) to %s", (unsigned)len, path);
    return ESP_OK;
}

/* ------------------------------ 流式读写 ------------------------------ */
/* 上传 / 下载动辄几十上百 MB，不能整块进内存：这里把"开一次、搬很多块、再关"暴露出去。
 * 写复用了 storage_write_all（非内部 RAM 的来源先过内部 DMA 缓冲），和整块写同一套。 */

struct svc_storage_writer_s {
    FILE *f;
};

esp_err_t svc_storage_write_open(const char *path, svc_storage_writer_t **out)
{
    if (path == NULL || out == NULL) return ESP_ERR_INVALID_ARG;
    *out = NULL;

    FILE *f = fopen(path, "wb");
    if (f == NULL) return ESP_FAIL;

    svc_storage_writer_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    w->f = f;
    *out = w;
    return ESP_OK;
}

esp_err_t svc_storage_write_chunk(svc_storage_writer_t *w, const void *data, size_t len)
{
    if (w == NULL || w->f == NULL) return ESP_ERR_INVALID_ARG;
    return storage_write_all(w->f, data, len);
}

esp_err_t svc_storage_write_close(svc_storage_writer_t *w)
{
    if (w == NULL) return ESP_ERR_INVALID_ARG;

    const bool ok = (w->f != NULL) && (fclose(w->f) == 0);
    free(w);
    return ok ? ESP_OK : ESP_FAIL;
}

/* 流式读的分块：8 KB —— 下发大文件时块大一点，socket 调用更少、走得快些 */
#define SVC_STORAGE_STREAM_CHUNK  8192

esp_err_t svc_storage_stream_read(const char *path, size_t *out_size,
                                  svc_storage_read_cb_t cb, void *user)
{
    if (path == NULL || cb == NULL) return ESP_ERR_INVALID_ARG;
    if (out_size != NULL) *out_size = 0;

    FILE *f = fopen(path, "rb");            /* 目录打不开：FATFS / SPIFFS 都会失败 */
    if (f == NULL) return ESP_ERR_NOT_FOUND;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    const long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    rewind(f);

    if (out_size != NULL) *out_size = (size_t)size;

    uint8_t *buf = malloc(SVC_STORAGE_STREAM_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    long left = size;
    while (left > 0) {
        const size_t want = (left < SVC_STORAGE_STREAM_CHUNK) ? (size_t)left
                                                             : SVC_STORAGE_STREAM_CHUNK;
        const size_t got = fread(buf, 1, want, f);
        if (got == 0) {                     /* 读不到就是出错（fread 会读到 EOF） */
            err = ESP_FAIL;
            break;
        }

        err = cb(buf, got, user);
        if (err != ESP_OK) break;           /* 客户端断开等：提前收工 */

        left -= (long)got;
    }

    free(buf);
    fclose(f);
    return err;
}

esp_err_t svc_storage_remove(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;

    if (remove(path) == 0) return ESP_OK;
    return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
}

/* 存储根目录不允许删 / 改名：它们由 periph_storage 挂载管理 */
static bool path_is_storage_root(const char *path)
{
    return strcmp(path, "/sdcard") == 0 || strcmp(path, "/internal") == 0;
}

esp_err_t svc_storage_rename(const char *from, const char *to)
{
    if (from == NULL || to == NULL) return ESP_ERR_INVALID_ARG;
    if (path_is_storage_root(from) || path_is_storage_root(to)) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(from, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (stat(to, &st) == 0) return ESP_ERR_INVALID_STATE;    /* 目标已存在，调用方换名 */

    if (rename(from, to) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (errno %d)", from, to, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t svc_storage_remove_tree(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (path_is_storage_root(path)) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;

    if (!S_ISDIR(st.st_mode)) {
        return (remove(path) == 0) ? ESP_OK : ESP_FAIL;
    }

    /* 目录：先递归删空里面的东西，再删目录本身 */
    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(path, &it) != ESP_OK) return ESP_FAIL;

    esp_err_t err = ESP_OK;
    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL) {
        char child[320];
        snprintf(child, sizeof(child), "%s/%s", path, entry->name);

        err = svc_storage_remove_tree(child);
        if (err != ESP_OK) break;       /* 出错就停，别继续往下删 */
    }
    svc_storage_iter_end(it);
    if (err != ESP_OK) return err;

    if (rmdir(path) != 0) {
        ESP_LOGW(TAG, "rmdir %s failed (errno %d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 分块大小：太小慢、太大吃内部 RAM（堆上分配，跨存储复制也够用） */
#define SVC_STORAGE_COPY_CHUNK  4096

static esp_err_t storage_copy_file(const char *from, const char *to)
{
    FILE *fs = fopen(from, "rb");
    if (fs == NULL) return ESP_ERR_NOT_FOUND;

    FILE *fd = fopen(to, "wb");
    if (fd == NULL) {
        fclose(fs);
        return ESP_FAIL;
    }

    uint8_t *chunk = malloc(SVC_STORAGE_COPY_CHUNK);
    if (chunk == NULL) {
        fclose(fs);
        fclose(fd);
        remove(to);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    while (true) {
        size_t n = fread(chunk, 1, SVC_STORAGE_COPY_CHUNK, fs);
        if (n > 0 && storage_write_all(fd, chunk, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        if (n < SVC_STORAGE_COPY_CHUNK) {
            if (ferror(fs)) err = ESP_FAIL;
            break;
        }
    }

    free(chunk);
    fclose(fs);
    if (fclose(fd) != 0) err = ESP_FAIL;

    /* 失败不留半个文件，免得下次复制把它当完整文件 */
    if (err != ESP_OK) remove(to);
    return err;
}

esp_err_t svc_storage_copy(const char *from, const char *to)
{
    if (from == NULL || to == NULL) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(from, &st) != 0) return ESP_ERR_NOT_FOUND;

    if (!S_ISDIR(st.st_mode)) return storage_copy_file(from, to);

    /* 目录：建出目标目录，再逐个递归复制（路径缓冲留在栈上，每层约 640 字节） */
    esp_err_t err = svc_storage_mkdir(to);
    if (err != ESP_OK) return err;

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(from, &it) != ESP_OK) return ESP_FAIL;

    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL) {
        char src[320];
        char dst[320];
        snprintf(src, sizeof(src), "%s/%s", from, entry->name);
        snprintf(dst, sizeof(dst), "%s/%s", to, entry->name);

        err = svc_storage_copy(src, dst);
        if (err != ESP_OK) break;
    }
    svc_storage_iter_end(it);
    return err;
}

esp_err_t svc_storage_exists(const char *path, size_t *out_size)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;

    if (out_size != NULL) *out_size = S_ISDIR(st.st_mode) ? 0 : (size_t)st.st_size;
    return ESP_OK;
}
