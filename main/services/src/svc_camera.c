/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Camera 实现（periph_camera 的薄封装）
 */

#include "svc_common.h"
#include "svc_camera.h"
#include "periph_common.h"
#include "esp_log.h"

static const char *TAG = "svc.camera";

static bool s_open = false;
static bool s_frame_held = false;
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
    if (s_open) return ESP_OK;

    const periph_camera_fmt_t pfmt = (fmt == SVC_CAMERA_FMT_JPEG) ? PERIPH_CAMERA_FMT_JPEG
                                                                  : PERIPH_CAMERA_FMT_RGB565;

    periph_camera_res_t pres;
    switch (size) {
    case SVC_CAMERA_SIZE_VGA:  pres = PERIPH_CAMERA_RES_VGA;  break;
    case SVC_CAMERA_SIZE_UXGA: pres = PERIPH_CAMERA_RES_UXGA; break;
    case SVC_CAMERA_SIZE_QVGA:
    default:                   pres = PERIPH_CAMERA_RES_QVGA; break;
    }

    esp_err_t err = periph_camera_init(pfmt, pres);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        return err;
    }

    s_open = true;
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
    return periph_camera_deinit();
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
