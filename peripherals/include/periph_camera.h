/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Camera
 *
 * 摄像头不在启动时初始化：由 App 打开相机界面时调用 periph_camera_init()，
 * 退出时 periph_camera_deinit()（会顺带把摄像头掉电）。PWDN 默认是掉电状态。
 */

#pragma once

#include "esp_err.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 打开摄像头（RGB565 / QVGA，2 帧缓冲在 PSRAM）
 */
esp_err_t periph_camera_init(void);

/**
 * @brief 关闭摄像头并掉电
 */
esp_err_t periph_camera_deinit(void);

/**
 * @brief 是否已打开
 */
bool periph_camera_is_ready(void);

/**
 * @brief 取一帧；用完必须 periph_camera_return_frame() 归还
 */
esp_err_t periph_camera_get_frame(camera_fb_t **out);

/**
 * @brief 归还一帧
 */
void periph_camera_return_frame(camera_fb_t *frame);

/**
 * @brief 切换分辨率
 */
esp_err_t periph_camera_set_framesize(framesize_t size);

#ifdef __cplusplus
}
#endif
