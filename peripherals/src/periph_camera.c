/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Camera 实现
 */

#include "periph_common.h"
#include "periph_camera.h"
#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "periph.camera";

static bool s_ready = false;

esp_err_t periph_camera_init(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = drv_gc0308_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera init failed (%s)", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t periph_camera_deinit(void)
{
    if (!s_ready) return ESP_OK;

    s_ready = false;
    ESP_LOGI(TAG, "deinitialized");
    return drv_gc0308_deinit();
}

bool periph_camera_is_ready(void)
{
    return s_ready;
}

esp_err_t periph_camera_get_frame(camera_fb_t **out)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    return drv_gc0308_get_frame(out);
}

void periph_camera_return_frame(camera_fb_t *frame)
{
    drv_gc0308_return_frame(frame);
}

esp_err_t periph_camera_set_framesize(framesize_t size)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    return drv_gc0308_set_framesize(size);
}

esp_err_t periph_camera_set_pixformat(pixformat_t format)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    return drv_gc0308_set_pixformat(format);
}
