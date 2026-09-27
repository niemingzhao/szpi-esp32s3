/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 音频（ES8311 DAC + ES7210 ADC）
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 音频方向
 */
typedef enum {
    PERIPH_AUDIO_DIR_PLAY,     // 播放
    PERIPH_AUDIO_DIR_RECORD,   // 录音
} periph_audio_dir_t;

/**
 * @brief 采样率
 */
typedef enum {
    PERIPH_AUDIO_SR_8K  = 8000,
    PERIPH_AUDIO_SR_16K = 16000,
    PERIPH_AUDIO_SR_22K = 22050,
    PERIPH_AUDIO_SR_32K = 32000,
    PERIPH_AUDIO_SR_44K = 44100,
    PERIPH_AUDIO_SR_48K = 48000,
} periph_audio_sample_rate_t;

/**
 * @brief 音频格式
 */
typedef struct {
    periph_audio_sample_rate_t sample_rate;
    uint8_t bit_width;        // 固定 16（I2S slot 与 codec 都是 16-bit）
    uint8_t channels;         // 1, 2
} periph_audio_format_t;

/**
 * @brief 初始化音频（ES8311 DAC + ES7210 ADC）
 */
esp_err_t periph_audio_init(void);

/**
 * @brief 设置播放 / 录音格式
 */
esp_err_t periph_audio_set_format(periph_audio_dir_t dir, const periph_audio_format_t *fmt);

/**
 * @brief 设置音量 0-100
 */
esp_err_t periph_audio_set_volume(uint8_t percent);

/**
 * @brief 获取当前音量
 */
uint8_t periph_audio_get_volume(void);

/**
 * @brief 静音开关
 */
esp_err_t periph_audio_set_mute(bool mute);

/**
 * @brief 写入 PCM 数据（播放方向）
 */
esp_err_t periph_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief 读取 PCM 数据（录音方向）
 */
esp_err_t periph_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief 重开 RX 通道
 *
 * 录音读失败（DMA 溢出 / 超时）后用：RX 只有 90 ms 出头的环形缓冲，TF 卡写卡顿一下
 * 就会溢出并让后续读一直失败，重开一次清掉溢出状态才能接着录。RX 是从机、跟着主
 * 通道的 BCLK / WS 走，重开它不影响播放侧的时钟。
 */
esp_err_t periph_audio_rx_restart(void);

#ifdef __cplusplus
}
#endif
