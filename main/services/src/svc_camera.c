/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 摄像头实现（periph_camera 的薄封装）
 */

#include "svc_common.h"
#include "svc_camera.h"
#include "periph_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "svc.camera";

static bool s_open = false;
static bool s_frame_held = false;
static svc_camera_format_t s_fmt = SVC_CAMERA_FMT_RGB565;
static svc_camera_size_t s_size = SVC_CAMERA_SIZE_QVGA;
/* 捕获时拿到的帧，release 时原样还回去 */
static periph_camera_frame_t s_frame = { 0 };

esp_err_t svc_camera_init(void)
{
    /* 硬件按需打开，这里只做服务侧状态复位 */
    s_open = false;
    s_frame_held = false;
    ESP_LOGI(TAG, "initialized (camera off)");
    return ESP_OK;
}

esp_err_t svc_camera_open(svc_camera_format_t fmt, svc_camera_size_t size)
{
    /* 参数没变就直接复用；变了必须关掉重开（驱动不支持原地切换格式 / 分辨率） */
    if (s_open) {
        if (fmt == s_fmt && size == s_size) return ESP_OK;
        svc_camera_close();
    }

    const periph_camera_fmt_t pfmt = (fmt == SVC_CAMERA_FMT_JPEG) ? PERIPH_CAMERA_FMT_JPEG
                                                                  : PERIPH_CAMERA_FMT_RGB565;

    periph_camera_res_t pres;
    switch (size) {
    case SVC_CAMERA_SIZE_VGA:  pres = PERIPH_CAMERA_RES_VGA;  break;
    case SVC_CAMERA_SIZE_UXGA: pres = PERIPH_CAMERA_RES_UXGA; break;
    case SVC_CAMERA_SIZE_QVGA:
    default:                   pres = PERIPH_CAMERA_RES_QVGA; break;
    }

    /* 相机要一整块 7.6 KB 连续内部 DMA，先把 Web 控制台（httpd 的任务栈）让出来 */
    svc_web_set_hold(true);

    esp_err_t err = periph_camera_init(pfmt, pres);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        svc_web_set_hold(false);
        return err;
    }

    s_open = true;
    s_fmt = fmt;
    s_size = size;
    ESP_LOGI(TAG, "opened (%s, size %d)",
             (fmt == SVC_CAMERA_FMT_JPEG) ? "JPEG" : "RGB565", (int)size);

    const bool opened = true;
    svc_event_bus_publish(SVC_EVENT_CAMERA_STATE_CHANGED, &opened, sizeof(opened));
    return ESP_OK;
}

esp_err_t svc_camera_close(void)
{
    if (!s_open) return ESP_OK;

    s_frame_held = false;
    s_open = false;

    const bool opened = false;
    svc_event_bus_publish(SVC_EVENT_CAMERA_STATE_CHANGED, &opened, sizeof(opened));
    const esp_err_t err = periph_camera_deinit();
    svc_web_set_hold(false);        /* 相机放开了，Web 控制台可以回来 */
    return err;
}

bool svc_camera_is_open(void)
{
    return s_open;
}

esp_err_t svc_camera_capture(svc_camera_frame_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_open) return ESP_ERR_INVALID_STATE;
    if (s_frame_held) return ESP_ERR_INVALID_STATE;   /* 上一帧没还，避免丢帧缓冲 */

    esp_err_t err = periph_camera_capture(&s_frame);
    if (err != ESP_OK) return err;

    s_frame_held = true;
    out->data = s_frame.buf;
    out->len = s_frame.buf_len;
    out->width = s_frame.width;
    out->height = s_frame.height;
    out->jpeg = (s_frame.fmt == PERIPH_CAMERA_FMT_JPEG);
    return ESP_OK;
}

void svc_camera_release(void)
{
    if (!s_frame_held) return;

    periph_camera_release_frame(&s_frame);
    s_frame_held = false;
}

/* ------------------------------- 帧 → BMP ------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* 摄像头出的 RGB565 是字节交换过的（LVGL 那边按 RGB565_SWAPPED 显示），
 * 写 BMP 前先换回来再拆 R/G/B */
static uint16_t pixel_rgb565(const uint16_t *line, int x)
{
    const uint16_t raw = line[x];
    return (uint16_t)((raw >> 8) | (raw << 8));
}

/* 支持的最大宽度（行缓冲按它开静态数组）：两个摄像头都出 QVGA，VGA 也够 */
#define SVC_CAMERA_BMP_MAX_W   640

esp_err_t svc_camera_write_bmp(const char *path, const uint16_t *rgb565, uint16_t w, uint16_t h)
{
    if (path == NULL || rgb565 == NULL || w == 0 || h == 0) return ESP_ERR_INVALID_ARG;
    if (w > SVC_CAMERA_BMP_MAX_W) {
        ESP_LOGW(TAG, "bmp width %u too large (max %d)", (unsigned)w, SVC_CAMERA_BMP_MAX_W);
        return ESP_ERR_INVALID_ARG;
    }

    /* 行缓冲放静态区（内部 RAM，SDMMC 可直接 DMA）：相机开着时内部 DMA 和 PSRAM 都紧，
     * 整幅 malloc 一张 200 KB 的图、或让存储层为 PSRAM 来源临时申请 1 KB 回弹缓冲，
     * 都可能失败。逐行写就只需要这一块固定的内部缓冲，不再有任何临时分配。 */
    static uint8_t s_row[SVC_CAMERA_BMP_MAX_W * 3];
    const size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;    /* 每行 4 字节对齐 */
    const size_t size = 54 + row * (size_t)h;

    svc_storage_writer_t *wr = NULL;
    esp_err_t err = svc_storage_write_open(path, &wr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %s failed: %s", path, esp_err_to_name(err));
        return err;
    }

    memset(s_row, 0, 54);
    s_row[0] = 'B';
    s_row[1] = 'M';
    put32(s_row + 2, (uint32_t)size);
    put32(s_row + 10, 54);
    put32(s_row + 14, 40);
    put32(s_row + 18, (uint32_t)w);
    put32(s_row + 22, (uint32_t)h);
    put16(s_row + 26, 1);
    put16(s_row + 28, 24);
    put32(s_row + 34, (uint32_t)(row * (size_t)h));
    err = svc_storage_write_chunk(wr, s_row, 54);

    /* BMP 自下而上：文件里的第一行是图像最底下一行，所以源的 y 要倒着写 */
    for (int y = 0; err == ESP_OK && y < (int)h; y++) {
        const uint16_t *line = rgb565 + (size_t)(h - 1 - y) * (size_t)w;

        memset(s_row, 0, row);                 /* 行尾对齐的填充字节留 0 */
        for (int x = 0; x < (int)w; x++) {
            const uint16_t px = pixel_rgb565(line, x);
            s_row[x * 3 + 0] = (uint8_t)((px & 0x1F) << 3);           /* B */
            s_row[x * 3 + 1] = (uint8_t)(((px >> 5) & 0x3F) << 2);    /* G */
            s_row[x * 3 + 2] = (uint8_t)(((px >> 11) & 0x1F) << 3);   /* R */
        }
        err = svc_storage_write_chunk(wr, s_row, row);
    }

    const esp_err_t cerr = svc_storage_write_close(wr);
    if (err == ESP_OK) err = cerr;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write %s failed: %s (dma-internal free=%u largest=%u)",
                 path, esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    return err;
}
