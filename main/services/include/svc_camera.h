/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 摄像头（按需开关）
 *
 * 只是 periph_camera 的薄封装：服务初始化不打开硬件，由 App / 脚本调
 * svc_camera_open() 打开、svc_camera_close() 关闭（关闭时给摄像头掉电）。
 * App 只看到 svc_camera_frame_t，不需要也不应该依赖 esp32-camera。
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
    SVC_CAMERA_SIZE_UXGA,        /* 1600 × 1200，仅 GC2145 */
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
 * @brief 服务初始化（不打开硬件，PWDN 保持掉电）
 */
esp_err_t svc_camera_init(void);

/**
 * @brief 打开摄像头（上电 + 按给定格式 / 分辨率初始化）
 *
 * 已打开且参数相同时为空操作；参数不同会先关掉再重开（驱动不支持原地切换）。
 */
esp_err_t svc_camera_open(svc_camera_format_t fmt, svc_camera_size_t size);

/**
 * @brief 关闭摄像头（掉电；上一帧随之失效，不必再 release）
 */
esp_err_t svc_camera_close(void);

/**
 * @brief 是否已打开
 */
bool svc_camera_is_open(void);

/**
 * @brief 取一帧；成功后必须 svc_camera_release() 归还，否则后续取帧会失败
 */
esp_err_t svc_camera_capture(svc_camera_frame_t *out);

/**
 * @brief 归还上一帧
 */
void svc_camera_release(void);

/**
 * @brief 把一段 RGB565 像素写成 24 位 BMP 文件（自下而上、BGR）
 *
 * 摄像头出的 RGB565 是字节交换过的（LVGL 那边按 RGB565_SWAPPED 显示），这里会换回来
 * 再拆 B/G/R，所以图库 App 与电脑都能直接打开。每行 4 字节对齐，逐行写、只占一块固定
 * 的内部行缓冲（相机开着时内部 DMA 与 PSRAM 都紧，不整幅 malloc）。相机 App 拍照与
 * 脚本的 camera.capture() 都走这里。
 *
 * @param[in] rgb565 像素起始地址（2 字节/像素）；JPEG 帧不能这么写
 * @param[in] w 宽度，最大 640
 * @param[in] h 高度
 */
esp_err_t svc_camera_write_bmp(const char *path, const uint16_t *rgb565, uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif
