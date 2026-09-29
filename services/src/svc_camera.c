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

/* 服务持有当前帧，直到 App 调 svc_camera_release() */
static camera_fb_t *s_fb = NULL;

esp_err_t svc_camera_init(void)
{
    if (periph_camera_is_ready()) return ESP_OK;

    esp_err_t err = periph_camera_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_camera_deinit(void)
{
    if (!periph_camera_is_ready()) return ESP_OK;

    if (s_fb != NULL) {
        periph_camera_return_frame(s_fb);
        s_fb = NULL;
    }
    return periph_camera_deinit();
}

bool svc_camera_is_ready(void)
{
    return periph_camera_is_ready();
}

esp_err_t svc_camera_set_format(svc_camera_format_t fmt)
{
    if (!periph_camera_is_ready()) return ESP_ERR_INVALID_STATE;

    pixformat_t f = (fmt == SVC_CAMERA_FMT_JPEG) ? PIXFORMAT_JPEG : PIXFORMAT_RGB565;
    return periph_camera_set_pixformat(f);
}

esp_err_t svc_camera_set_size(svc_camera_size_t size)
{
    if (!periph_camera_is_ready()) return ESP_ERR_INVALID_STATE;

    framesize_t f = (size == SVC_CAMERA_SIZE_VGA) ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
    return periph_camera_set_framesize(f);
}

esp_err_t svc_camera_capture(svc_camera_frame_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!periph_camera_is_ready()) return ESP_ERR_INVALID_STATE;
    if (s_fb != NULL) return ESP_ERR_INVALID_STATE;   /* 上一帧没还，避免丢帧缓冲 */

    esp_err_t err = periph_camera_get_frame(&s_fb);
    if (err != ESP_OK || s_fb == NULL) {
        s_fb = NULL;
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    out->data = s_fb->buf;
    out->len = s_fb->len;
    out->width = (uint16_t)s_fb->width;
    out->height = (uint16_t)s_fb->height;
    out->jpeg = (s_fb->format == PIXFORMAT_JPEG);
    return ESP_OK;
}

void svc_camera_release(void)
{
    if (s_fb == NULL) return;

    periph_camera_return_frame(s_fb);
    s_fb = NULL;
}
