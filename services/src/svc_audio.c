/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Audio 实现
 *
 * play_task 串行处理播放请求：
 *   WAV（PCM 16-bit，单 / 双声道）→ periph_audio_write
 *   TONE                        → 正弦波合成
 * MP3（需解码器组件）、URL / 流、录音（ES7210）尚未支持。
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "svc.audio";

#define AUDIO_NS            "sys"
#define AUDIO_VOL_KEY       "volume"
#define AUDIO_TASK_STACK    4096
#define AUDIO_TASK_PRIO     6
#define AUDIO_TASK_CORE     1
#define AUDIO_CHUNK         4096     /* 单次读取字节数 */
#define AUDIO_URI_MAX       192
#define AUDIO_SR_DEFAULT    PERIPH_AUDIO_SR_16K
#define AUDIO_PI            3.14159265f

typedef struct {
    svc_audio_src_type_t type;
    char uri[AUDIO_URI_MAX];
    uint16_t tone_freq;
    uint32_t tone_duration_ms;
} audio_msg_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile svc_audio_state_t s_state = SVC_AUDIO_STATE_IDLE;
static volatile bool s_stop_req = false;
static volatile bool s_pause_req = false;

static uint8_t s_volume = 80;
static bool s_muted = false;
static bool s_ready = false;

static svc_audio_cb_t s_cb = NULL;
static void *s_cb_user = NULL;

/* ------------------------------ 状态与回调 ------------------------------ */

static void notify(svc_audio_evt_t evt, svc_audio_state_t st, esp_err_t err)
{
    if (s_cb == NULL) return;

    svc_audio_evt_data_t d;
    memset(&d, 0, sizeof(d));
    d.evt = evt;
    if (evt == SVC_AUDIO_EVT_STATE_CHANGED) d.new_state = st;
    else d.error = err;

    s_cb(&d, s_cb_user);
}

static void enter_state(svc_audio_state_t st)
{
    s_state = st;
    notify(SVC_AUDIO_EVT_STATE_CHANGED, st, ESP_OK);
}

/* -------------------------------- WAV 解析 -------------------------------- */

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t data_size;
} wav_info_t;

static bool wav_parse(FILE *f, wav_info_t *info)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    uint8_t chunk[8];
    while (fread(chunk, 1, sizeof(chunk), f) == sizeof(chunk)) {
        uint32_t size = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8)
                      | ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt)) return false;

            uint16_t audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));
            info->channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            info->sample_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8)
                              | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            uint16_t bits = (uint16_t)(fmt[14] | (fmt[15] << 8));

            if (audio_format != 1 || bits != 16) return false;   /* 仅 PCM 16-bit */
            if (size > sizeof(fmt)) fseek(f, (long)(size - sizeof(fmt)), SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_size = size;
            return true;
        } else {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);        /* 块按偶数字节对齐 */
        }
    }
    return false;
}

static esp_err_t play_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    wav_info_t wav = { 0 };
    if (!wav_parse(f, &wav) || wav.data_size == 0) {
        ESP_LOGE(TAG, "unsupported WAV (need PCM 16-bit): %s", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    periph_audio_format_t fmt = {
        .sample_rate = (periph_audio_sample_rate_t)wav.sample_rate,
        .bit_width = 16,
        .channels = (uint8_t)(wav.channels == 1 ? 1 : 2),
    };
    esp_err_t err = periph_audio_set_format(PERIPH_AUDIO_DIR_PLAY, &fmt);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    uint8_t *buf = malloc(AUDIO_CHUNK * 2);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    if (!s_muted) periph_audio_set_mute(false);      /* 打开功放 */

    ESP_LOGI(TAG, "playing %s (%u Hz, %u ch)", path, (unsigned)wav.sample_rate, wav.channels);
    enter_state(SVC_AUDIO_STATE_PLAYING);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_STARTED, NULL, 0);

    uint32_t remaining = wav.data_size;
    bool ok = true;

    while (remaining > 0 && !s_stop_req) {
        while (s_pause_req && !s_stop_req) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_stop_req) break;

        size_t want = (remaining > AUDIO_CHUNK) ? AUDIO_CHUNK : remaining;
        size_t rd = fread(buf, 1, want, f);
        if (rd == 0) break;
        remaining -= (uint32_t)rd;

        size_t out_len = rd;
        if (wav.channels == 1) {
            size_t frames = rd / 2;
            for (size_t i = frames; i > 0; i--) {
                int16_t s = ((int16_t *)buf)[i - 1];
                ((int16_t *)buf)[2 * (i - 1)] = s;
                ((int16_t *)buf)[2 * (i - 1) + 1] = s;
            }
            out_len = frames * 4;
        }

        if (periph_audio_write(buf, out_len, 0) != ESP_OK) {
            ok = false;
            break;
        }
    }

    free(buf);
    fclose(f);

    periph_audio_set_mute(true);                     /* 关闭功放 */

    if (!ok) {
        notify(SVC_AUDIO_EVT_PLAYBACK_ERROR, SVC_AUDIO_STATE_IDLE, ESP_FAIL);
        svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_ERROR, NULL, 0);
    }
    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, NULL, 0);

    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t play_tone(uint16_t freq, uint32_t ms)
{
    if (freq == 0) freq = 1000;
    if (ms == 0) ms = 200;

    periph_audio_format_t fmt = {
        .sample_rate = AUDIO_SR_DEFAULT, .bit_width = 16, .channels = 2,
    };
    esp_err_t err = periph_audio_set_format(PERIPH_AUDIO_DIR_PLAY, &fmt);
    if (err != ESP_OK) return err;

    uint8_t *buf = malloc(AUDIO_CHUNK * 2);
    if (buf == NULL) return ESP_ERR_NO_MEM;

    if (!s_muted) periph_audio_set_mute(false);

    enter_state(SVC_AUDIO_STATE_PLAYING);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_STARTED, NULL, 0);

    const uint32_t sr = (uint32_t)AUDIO_SR_DEFAULT;
    uint32_t total = (uint32_t)((uint64_t)sr * ms / 1000);
    int16_t *pcm = (int16_t *)buf;
    uint32_t done = 0;

    while (done < total && !s_stop_req) {
        uint32_t n = total - done;
        if (n > AUDIO_CHUNK / 4) n = AUDIO_CHUNK / 4;

        for (uint32_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(sinf(2.0f * AUDIO_PI * (float)freq * (float)(done + i) / (float)sr) * 10000.0f);
            pcm[2 * i] = v;
            pcm[2 * i + 1] = v;
        }

        if (periph_audio_write(buf, n * 4, 0) != ESP_OK) break;
        done += n;
    }

    free(buf);
    periph_audio_set_mute(true);
    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, NULL, 0);
    return ESP_OK;
}

/* -------------------------------- play_task -------------------------------- */

static void audio_task(void *arg)
{
    (void)arg;
    audio_msg_t msg;

    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        s_stop_req = false;
        s_pause_req = false;

        if (msg.type == SVC_AUDIO_SRC_FILE) {
            play_file(msg.uri);
        } else if (msg.type == SVC_AUDIO_SRC_TONE) {
            play_tone(msg.tone_freq, msg.tone_duration_ms);
        } else {
            ESP_LOGW(TAG, "unsupported source type %d", msg.type);
            enter_state(SVC_AUDIO_STATE_IDLE);
            svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_ERROR, NULL, 0);
        }
    }
}

/* ---------------------------------- API ---------------------------------- */

esp_err_t svc_audio_init(void)
{
    esp_err_t err = periph_audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "periph audio init failed (%s)，音频不可用", esp_err_to_name(err));
        return ESP_OK;   /* 不阻塞启动 */
    }

    uint8_t vol = 80;
    svc_settings_get_u8(AUDIO_NS, AUDIO_VOL_KEY, &vol, 80);
    s_volume = vol;
    periph_audio_set_volume(s_volume);

    s_queue = xQueueCreate(1, sizeof(audio_msg_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "svc_audio", AUDIO_TASK_STACK, NULL,
                                            AUDIO_TASK_PRIO, &s_task, AUDIO_TASK_CORE);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    s_ready = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_audio_deinit(void)
{
    s_stop_req = true;
    s_pause_req = false;
    return ESP_OK;
}

esp_err_t svc_audio_play(const svc_audio_source_t *src)
{
    if (src == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    if (src->type != SVC_AUDIO_SRC_FILE && src->type != SVC_AUDIO_SRC_TONE) {
        ESP_LOGW(TAG, "source type %d not supported yet", src->type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (src->type == SVC_AUDIO_SRC_FILE && src->uri == NULL) return ESP_ERR_INVALID_ARG;

    audio_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = src->type;
    msg.tone_freq = src->tone_freq;
    msg.tone_duration_ms = src->tone_duration_ms;
    if (src->uri != NULL) {
        strlcpy(msg.uri, src->uri, sizeof(msg.uri));
    }

    s_stop_req = true;                       /* 打断当前播放 */
    if (xQueueOverwrite(s_queue, &msg) != pdTRUE) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t svc_audio_pause(void)
{
    if (s_state != SVC_AUDIO_STATE_PLAYING) return ESP_ERR_INVALID_STATE;
    s_pause_req = true;
    enter_state(SVC_AUDIO_STATE_PAUSED);
    return ESP_OK;
}

esp_err_t svc_audio_resume(void)
{
    if (s_state != SVC_AUDIO_STATE_PAUSED) return ESP_ERR_INVALID_STATE;
    s_pause_req = false;
    enter_state(SVC_AUDIO_STATE_PLAYING);
    return ESP_OK;
}

esp_err_t svc_audio_stop(void)
{
    s_stop_req = true;
    s_pause_req = false;
    return ESP_OK;
}

esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds)
{
    (void)file_path;
    (void)max_seconds;
    return ESP_ERR_NOT_SUPPORTED;   /* ES7210 + I2S RX 尚未接入 */
}

esp_err_t svc_audio_record_stop(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t svc_audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume = percent;

    if (s_ready) {
        periph_audio_set_volume(percent);
        svc_settings_set_u8(AUDIO_NS, AUDIO_VOL_KEY, percent);
    }
    return ESP_OK;
}

uint8_t svc_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t svc_audio_set_mute(bool mute)
{
    s_muted = mute;
    if (s_ready) periph_audio_set_mute(mute);
    return ESP_OK;
}

svc_audio_state_t svc_audio_get_state(void)
{
    return s_state;
}

esp_err_t svc_audio_register_callback(svc_audio_cb_t cb, void *user)
{
    s_cb = cb;
    s_cb_user = user;
    return ESP_OK;
}

esp_err_t svc_audio_play_tone_async(uint16_t freq_hz, uint32_t ms)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    svc_audio_source_t src;
    memset(&src, 0, sizeof(src));
    src.type = SVC_AUDIO_SRC_TONE;
    src.tone_freq = freq_hz;
    src.tone_duration_ms = ms;
    return svc_audio_play(&src);
}
