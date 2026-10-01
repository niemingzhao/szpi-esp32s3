/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Camera
 *
 * 摄像头不在启动时初始化：由 svc_camera 在打开相机时调用
 * periph_camera_init(fmt, res)，关闭时 periph_camera_deinit()（顺带把摄像头掉电）。
 * PWDN 默认是掉电状态。esp32-camera 的类型不透出给 Services / Apps。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PERIPH_CAMERA_FMT_RGB565,
    PERIPH_CAMERA_FMT_JPEG,
} periph_camera_fmt_t;

typedef enum {
    PERIPH_CAMERA_RES_QVGA = 0,    /* 320×240 */
    PERIPH_CAMERA_RES_VGA,         /* 640×480 */
    PERIPH_CAMERA_RES_UXGA,        /* 1600×1200，仅 GC2145 */
} periph_camera_res_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    periph_camera_fmt_t fmt;
    uint8_t *buf;
    size_t buf_len;
} periph_camera_frame_t;

/**
 * @brief 打开摄像头（按 PID 自动识别 GC0308 / GC2145）
 */
esp_err_t periph_camera_init(periph_camera_fmt_t fmt, periph_camera_res_t res);

/**
 * @brief 关闭摄像头并掉电
 */
esp_err_t periph_camera_deinit(void);

/**
 * @brief 抓取一帧（同步）；用完必须 periph_camera_release_frame() 归还
 */
esp_err_t periph_camera_capture(periph_camera_frame_t *frame);

/**
 * @brief 归还一帧
 */
esp_err_t periph_camera_release_frame(periph_camera_frame_t *frame);

#ifdef __cplusplus
}
#endif
