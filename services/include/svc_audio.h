/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Audio
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
 * @brief 音频来源类型
 */
typedef enum {
    SVC_AUDIO_SRC_FILE,     // 文件路径
    SVC_AUDIO_SRC_URL,      // HTTP URL
    SVC_AUDIO_SRC_TONE,     // 简单 tone
    SVC_AUDIO_SRC_STREAM,   // 外部 PCM 流
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
    bool loop;
} svc_audio_source_t;

/**
 * @brief 音频事件
 */
typedef enum {
    SVC_AUDIO_EVT_STATE_CHANGED,
    SVC_AUDIO_EVT_PLAYBACK_FINISHED,
    SVC_AUDIO_EVT_PLAYBACK_ERROR,
    SVC_AUDIO_EVT_RECORD_FINISHED,
} svc_audio_evt_t;

typedef struct {
    svc_audio_evt_t evt;
    union {
        svc_audio_state_t new_state;
        esp_err_t error;
    };
} svc_audio_evt_data_t;

typedef void (*svc_audio_cb_t)(const svc_audio_evt_data_t *evt, void *user);

esp_err_t svc_audio_init(void);
esp_err_t svc_audio_deinit(void);

esp_err_t svc_audio_play(const svc_audio_source_t *src);
esp_err_t svc_audio_pause(void);
esp_err_t svc_audio_resume(void);
esp_err_t svc_audio_stop(void);

esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds);
esp_err_t svc_audio_record_stop(void);

esp_err_t svc_audio_set_volume(uint8_t percent);
uint8_t svc_audio_get_volume(void);
esp_err_t svc_audio_set_mute(bool mute);

svc_audio_state_t svc_audio_get_state(void);

esp_err_t svc_audio_register_callback(svc_audio_cb_t cb, void *user);

/** 异步播放 tone（不打断当前音频） */
esp_err_t svc_audio_play_tone_async(uint16_t freq_hz, uint32_t ms);

#ifdef __cplusplus
}
#endif
