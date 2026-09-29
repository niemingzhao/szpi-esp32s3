/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Camera（按需开关的摄像头服务）
 *
 * 只做 periph_camera 的适配：把 esp32-camera 的类型挡在 Services 层里，
 * App 只看到 svc_camera_frame_t，不需要（也不应该）直接依赖 ESP-IDF / 组件头文件。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 像素格式 */
typedef enum {
    SVC_CAMERA_FMT_RGB565 = 0,   /* 预览用（LVGL 可直接显示） */
    SVC_CAMERA_FMT_JPEG,         /* 拍照用（直接写文件） */
} svc_camera_format_t;

/** 分辨率 */
typedef enum {
    SVC_CAMERA_SIZE_QVGA = 0,    /* 320 × 240 */
    SVC_CAMERA_SIZE_VGA,         /* 640 × 480 */
} svc_camera_size_t;

/** 一帧画面（数据由驱动持有，svc_camera_release() 之后失效） */
typedef struct {
    const uint8_t *data;
    size_t len;
    uint16_t width;
    uint16_t height;
    bool jpeg;
} svc_camera_frame_t;

/**
 * @brief 打开摄像头（PWDN 上电 + 初始化）
 */
esp_err_t svc_camera_init(void);

/**
 * @brief 关闭摄像头（掉电）
 */
esp_err_t svc_camera_deinit(void);

/**
 * @brief 是否已打开
 */
bool svc_camera_is_ready(void);

/**
 * @brief 切换像素格式 / 分辨率
 */
esp_err_t svc_camera_set_format(svc_camera_format_t fmt);
esp_err_t svc_camera_set_size(svc_camera_size_t size);

/**
 * @brief 取一帧（RGB565 或 JPEG，由当前格式决定）
 *
 * 成功后必须调用 svc_camera_release() 归还帧缓冲，否则后续取帧会一直失败。
 */
esp_err_t svc_camera_capture(svc_camera_frame_t *out);

/**
 * @brief 归还上一帧
 */
void svc_camera_release(void);

#ifdef __cplusplus
}
#endif
