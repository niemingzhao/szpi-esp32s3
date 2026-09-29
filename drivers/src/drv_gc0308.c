/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - GC0308 摄像头（DVP + SCCB）
 *
 * 引脚见 docs/01-requirements/01-hardware-spec.md：D0~D7 = 16/18/8/17/15/6/4/9，
 * XCLK = 5，PCLK = 7，VSYNC = 3，HREF = 46，SIOC/SIOD 复用 GPIO2/GPIO1（I2C0 的引脚）。
 * PWDN 由 PCA9557.BIT2 控制（低有效），RST 为 NC。
 *
 * SCCB 用新版 i2c_master 驱动、走 I2C1（引脚仍复用 GPIO1/2，与 I2C0 各自独立控制器，
 * 互不占用软件资源）。Kconfig 默认就是 NEW 驱动 + PORT1，这里在 sdkconfig.defaults 里
 * 显式写死，避免以后组件改默认值。
 *
 * 输出格式：RGB565 / QVGA，2 个帧缓冲放 PSRAM，抓取模式 CAMERA_GRAB_WHEN_EMPTY。
 * XCLK 由 LEDC 产生，占用 TIMER_1 / CHANNEL_1（TIMER_0 / CHANNEL_0 留给背光）。
 */

#include "drv_common.h"
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "drv.camera";

#define DRV_CAM_PIN_D0      GPIO_NUM_16
#define DRV_CAM_PIN_D1      GPIO_NUM_18
#define DRV_CAM_PIN_D2      GPIO_NUM_8
#define DRV_CAM_PIN_D3      GPIO_NUM_17
#define DRV_CAM_PIN_D4      GPIO_NUM_15
#define DRV_CAM_PIN_D5      GPIO_NUM_6
#define DRV_CAM_PIN_D6      GPIO_NUM_4
#define DRV_CAM_PIN_D7      GPIO_NUM_9
#define DRV_CAM_PIN_XCLK    GPIO_NUM_5
#define DRV_CAM_PIN_PCLK    GPIO_NUM_7
#define DRV_CAM_PIN_VSYNC   GPIO_NUM_3
#define DRV_CAM_PIN_HREF    GPIO_NUM_46
#define DRV_CAM_PIN_SIOC    GPIO_NUM_2
#define DRV_CAM_PIN_SIOD    GPIO_NUM_1

#define DRV_CAM_XCLK_HZ     24000000
#define DRV_CAM_SCCB_PORT   1

static bool s_initialized = false;

esp_err_t drv_gc0308_init(void)
{
    if (s_initialized) return ESP_OK;

    /* PWDN 低有效：先给摄像头模块上电 */
    drv_pca9557_set_pin(DRV_PCA9557_DVP_PWDN, 0);

    const camera_config_t cfg = {
        .pin_pwdn = -1,                 /* 由 PCA9557.BIT2 控制，不占 GPIO */
        .pin_reset = -1,                /* RST 为 NC */
        .pin_xclk = DRV_CAM_PIN_XCLK,
        .pin_sccb_sda = DRV_CAM_PIN_SIOD,
        .pin_sccb_scl = DRV_CAM_PIN_SIOC,
        .sccb_i2c_port = DRV_CAM_SCCB_PORT,
        .pin_d7 = DRV_CAM_PIN_D7,
        .pin_d6 = DRV_CAM_PIN_D6,
        .pin_d5 = DRV_CAM_PIN_D5,
        .pin_d4 = DRV_CAM_PIN_D4,
        .pin_d3 = DRV_CAM_PIN_D3,
        .pin_d2 = DRV_CAM_PIN_D2,
        .pin_d1 = DRV_CAM_PIN_D1,
        .pin_d0 = DRV_CAM_PIN_D0,
        .pin_vsync = DRV_CAM_PIN_VSYNC,
        .pin_href = DRV_CAM_PIN_HREF,
        .pin_pclk = DRV_CAM_PIN_PCLK,
        .xclk_freq_hz = DRV_CAM_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_1,     /* 避开背光的 TIMER_0 */
        .ledc_channel = LEDC_CHANNEL_1, /* 避开背光的 CHANNEL_0 */
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        drv_pca9557_set_pin(DRV_PCA9557_DVP_PWDN, 1);   /* 失败就掉电 */
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        ESP_LOGI(TAG, "initialized (PID 0x%02x, RGB565 %ux%u, %d fb in PSRAM)",
                 s->id.PID, DRV_LCD_V_RES, DRV_LCD_H_RES, cfg.fb_count);
    } else {
        ESP_LOGW(TAG, "initialized, but sensor info unavailable");
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t drv_gc0308_get_frame(camera_fb_t **out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    *out = esp_camera_fb_get();
    return (*out != NULL) ? ESP_OK : ESP_FAIL;
}

void drv_gc0308_return_frame(camera_fb_t *frame)
{
    if (frame != NULL) {
        esp_camera_fb_return(frame);
    }
}

esp_err_t drv_gc0308_set_framesize(framesize_t size)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_framesize == NULL) return ESP_FAIL;

    return (s->set_framesize(s, size) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t drv_gc0308_set_pixformat(pixformat_t format)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_pixformat == NULL) return ESP_FAIL;

    return (s->set_pixformat(s, format) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t drv_gc0308_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    esp_err_t err = esp_camera_deinit();
    drv_pca9557_set_pin(DRV_PCA9557_DVP_PWDN, 1);   /* 掉电 */
    s_initialized = false;
    return err;
}
