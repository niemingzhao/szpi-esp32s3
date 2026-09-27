/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 音频（播放 / 录音 / 音量）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 采样率（录音只支持 16 / 44.1 / 48 kHz）
 */
typedef enum {
    SVC_AUDIO_SR_8K  = 8000,
    SVC_AUDIO_SR_16K = 16000,
    SVC_AUDIO_SR_22K = 22050,
    SVC_AUDIO_SR_32K = 32000,
    SVC_AUDIO_SR_44K = 44100,
    SVC_AUDIO_SR_48K = 48000,
} svc_audio_sample_rate_t;

/**
 * @brief PCM 格式（外部流用；bit_width 固定 16）
 */
typedef struct {
    svc_audio_sample_rate_t sample_rate;
    uint8_t bit_width;
    uint8_t channels;       /* 1 / 2 */
} svc_audio_format_t;

/**
 * @brief 音频来源类型
 */
typedef enum {
    SVC_AUDIO_SRC_FILE,     // 文件路径（支持 MP3 / PCM WAV）
    SVC_AUDIO_SRC_URL,      // HTTP(S) 链接（支持，边下边播 MP3）
    SVC_AUDIO_SRC_TONE,     // 简单 tone
    SVC_AUDIO_SRC_STREAM,   // 外部 PCM 流（调用方经 svc_audio_write 喂数据）
} svc_audio_src_type_t;

/**
 * @brief 播放状态
 */
typedef enum {
    SVC_AUDIO_STATE_IDLE,
    SVC_AUDIO_STATE_PLAYING,
    SVC_AUDIO_STATE_PAUSED,
    SVC_AUDIO_STATE_RECORDING,
} svc_audio_state_t;

/**
 * @brief 音频来源
 */
typedef struct {
    svc_audio_src_type_t type;
    const char *uri;
    uint16_t tone_freq;
    uint32_t tone_duration_ms;
} svc_audio_source_t;

esp_err_t svc_audio_init(void);

esp_err_t svc_audio_play(const svc_audio_source_t *src);
esp_err_t svc_audio_pause(void);
esp_err_t svc_audio_resume(void);
esp_err_t svc_audio_stop(void);

/**
 * @brief 开始录音（max_seconds 为 0 时按 60 s，上限 1 小时；写成 PCM WAV）
 */
esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds);
esp_err_t svc_audio_record_stop(void);

/**
 * @brief 打开外部 PCM 流播放通路
 *
 * 与文件 / 链接播放互斥：调用前应确保当前没有播放或录音。打开后由调用方自己
 * 产生 PCM，经 svc_audio_write 直接写 I2S，直到 svc_audio_stream_stop。
 * fmt 只支持 16-bit、1 / 2 声道；单声道数据由调用方复制成双声道。
 */
esp_err_t svc_audio_stream_start(const svc_audio_format_t *fmt);

/**
 * @brief 向已打开的外部 PCM 流写入数据（内部转发 periph_audio_write）
 *
 * 未打开流 / 已被 stop 或新播放请求打断时返回 ESP_ERR_INVALID_STATE，
 * 调用方据此结束自己产生数据的循环。
 */
esp_err_t svc_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief 关闭外部 PCM 流（静音并回到 IDLE，发布播放结束事件）
 */
esp_err_t svc_audio_stream_stop(void);

esp_err_t svc_audio_set_volume(uint8_t percent);
uint8_t svc_audio_get_volume(void);
esp_err_t svc_audio_set_mute(bool mute);

/** 当前是否静音（界面进入时同步用） */
bool svc_audio_get_mute(void);

svc_audio_state_t svc_audio_get_state(void);

/** 异步播放 tone（经播放任务排队，会打断当前播放；调用方不阻塞） */
esp_err_t svc_audio_play_tone_async(uint16_t freq_hz, uint32_t ms);

#ifdef __cplusplus
}
#endif
