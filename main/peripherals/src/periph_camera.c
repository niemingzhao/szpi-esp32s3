/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Camera 实现
 *
 * esp32-camera 的 camera_fb_t 只在层内流转：对外是 periph_camera_frame_t，
 * 归还时用捕获时记下的 fb 指针（同一时刻只允许一帧在外）。
 */

#include "periph_common.h"
#include "periph_camera.h"
#include "drv_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_camera.h"

static const char *TAG = "periph.camera";

static bool s_ready = false;
static camera_fb_t *s_fb = NULL;

static framesize_t res_to_framesize(periph_camera_res_t res)
{
    switch (res) {
    case PERIPH_CAMERA_RES_VGA:  return FRAMESIZE_VGA;
    case PERIPH_CAMERA_RES_UXGA: return FRAMESIZE_UXGA;
    case PERIPH_CAMERA_RES_QVGA:
    default:                     return FRAMESIZE_QVGA;
    }
}

esp_err_t periph_camera_init(periph_camera_fmt_t fmt, periph_camera_res_t res)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = drv_camera_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera init failed (%s)", esp_err_to_name(err));
        return err;
    }

    err = drv_camera_set_framesize(res_to_framesize(res));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set framesize failed (%s)", esp_err_to_name(err));
        drv_camera_deinit();
        return err;
    }

    err = drv_camera_set_pixformat((fmt == PERIPH_CAMERA_FMT_JPEG) ? PIXFORMAT_JPEG
                                                                : PIXFORMAT_RGB565);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set pixformat failed (%s)", esp_err_to_name(err));
        drv_camera_deinit();
        return err;
    }

    s_fb = NULL;
    s_ready = true;
    ESP_LOGI(TAG, "initialized (%s, res %d)",
             (fmt == PERIPH_CAMERA_FMT_JPEG) ? "JPEG" : "RGB565", (int)res);
    return ESP_OK;
}

esp_err_t periph_camera_deinit(void)
{
    if (!s_ready) return ESP_OK;

    if (s_fb != NULL) {
        drv_camera_return_frame(s_fb);
        s_fb = NULL;
    }
    s_ready = false;
    ESP_LOGI(TAG, "deinitialized");
    return drv_camera_deinit();
}

esp_err_t periph_camera_capture(periph_camera_frame_t *frame)
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_fb != NULL) return ESP_ERR_INVALID_STATE;   /* 上一帧还没归还 */

    camera_fb_t *fb = NULL;
    esp_err_t err = drv_camera_get_frame(&fb);
    if (err != ESP_OK || fb == NULL) {
        return ESP_FAIL;
    }

    s_fb = fb;
    frame->width = (uint16_t)fb->width;
    frame->height = (uint16_t)fb->height;
    frame->fmt = (fb->format == PIXFORMAT_JPEG) ? PERIPH_CAMERA_FMT_JPEG
                                                : PERIPH_CAMERA_FMT_RGB565;
    frame->buf = fb->buf;
    frame->buf_len = fb->len;
    return ESP_OK;
}

esp_err_t periph_camera_release_frame(periph_camera_frame_t *frame)
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    if (s_fb == NULL) return ESP_ERR_INVALID_STATE;

    drv_camera_return_frame(s_fb);
    s_fb = NULL;

    frame->buf = NULL;
    frame->buf_len = 0;
    return ESP_OK;
}
