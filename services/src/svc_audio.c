/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Audio 实现
 *
 * play_task 串行处理播放 / 录音请求：
 *   WAV（PCM 16-bit，单 / 双声道）→ periph_audio_write
 *   TONE                        → 正弦波合成
 *   录音（ES7210）               → periph_audio_read 写入 WAV 文件
 * MP3（需解码器组件）、URL / 流尚未支持。
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "mp3dec.h"
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
    bool record;             /* 本次请求是录音（uri 为目标文件，max_seconds 为时长上限） */
    uint32_t max_seconds;
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

/* -------------------------------- MP3 播放 -------------------------------- */

#define MP3_IN_BUF          (8 * 1024)                          /* 输入缓冲 */
#define MP3_OUT_SAMPLES     (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)  /* 单帧最大 PCM 样点数 */
#define MP3_REFILL_WATER    4096                                /* 缓冲低于此值就补数据 */

static bool has_suffix(const char *path, const char *suffix)
{
    size_t lp = strlen(path);
    size_t ls = strlen(suffix);
    if (lp < ls) return false;

    for (size_t i = 0; i < ls; i++) {
        char a = path[lp - ls + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/* helix 解码 → 16-bit PCM → periph_audio_write；单声道帧复制成双声道 */
static esp_err_t play_mp3(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    HMP3Decoder dec = MP3InitDecoder();
    uint8_t *in = malloc(MP3_IN_BUF);
    int16_t *out = malloc(MP3_OUT_SAMPLES * sizeof(int16_t));
    if (dec == NULL || in == NULL || out == NULL) {
        ESP_LOGE(TAG, "mp3 buffer alloc failed");
        if (dec != NULL) MP3FreeDecoder(dec);
        free(in);
        free(out);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    uint8_t *p = in;                 /* 当前解析位置 */
    int left = 0;                    /* 缓冲中剩余字节 */
    bool eof = false;
    int cur_rate = 0;
    uint32_t samples_out = 0;

    ESP_LOGI(TAG, "playing mp3 %s", path);
    enter_state(SVC_AUDIO_STATE_PLAYING);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_STARTED, NULL, 0);
    if (!s_muted) periph_audio_set_mute(false);

    while (!s_stop_req) {
        while (s_pause_req && !s_stop_req) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_stop_req) break;

        /* 补数据：把还没解析的部分搬到缓冲开头，再读满 */
        if (left < MP3_REFILL_WATER && !eof) {
            if (left > 0) memmove(in, p, (size_t)left);
            size_t rd = fread(in + left, 1, MP3_IN_BUF - (size_t)left, f);
            if (rd == 0) eof = true;
            left += (int)rd;
            p = in;
        }
        if (left <= 0) break;                          /* 文件读完 */

        int off = MP3FindSyncWord(p, left);
        if (off < 0) {
            left = 0;                                  /* 没有同步字：整段丢掉继续读 */
            if (eof) break;
            continue;
        }
        p += off;
        left -= off;

        int err = MP3Decode(dec, &p, &left, out, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof) break;
            continue;                                  /* 补数据后再解 */
        }
        if (err != ERR_MP3_NONE) {
            /* 坏帧：往后挪一字节，避免下一次又找到同一帧原地打转 */
            if (left > 0) {
                p += 1;
                left -= 1;
            }
            continue;
        }

        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        if (info.samprate <= 0 || info.nChans <= 0) continue;

        /* 采样率变了就重配 I2S + ES8311（44.1 k / 48 k / 16 k 都支持） */
        if (info.samprate != cur_rate) {
            periph_audio_format_t fmt = {
                .sample_rate = (periph_audio_sample_rate_t)info.samprate,
                .bit_width = 16,
                .channels = (uint8_t)(info.nChans == 1 ? 1 : 2),
            };
            esp_err_t ferr = periph_audio_set_format(PERIPH_AUDIO_DIR_PLAY, &fmt);
            if (ferr != ESP_OK) {
                ESP_LOGW(TAG, "mp3 %d Hz not supported: %s", info.samprate, esp_err_to_name(ferr));
                ret = ferr;
                break;
            }
            cur_rate = info.samprate;
        }

        int frames = info.outputSamps / info.nChans;
        if (info.nChans == 1) {
            for (int i = frames; i > 0; i--) {
                int16_t s = out[i - 1];
                out[2 * (i - 1)] = s;
                out[2 * (i - 1) + 1] = s;
            }
        }

        if (periph_audio_write((const uint8_t *)out,
                               (size_t)frames * 2 * sizeof(int16_t), 0) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
        samples_out += (uint32_t)frames;
    }

    free(out);
    free(in);
    MP3FreeDecoder(dec);
    fclose(f);

    periph_audio_set_mute(true);

    if (ret != ESP_OK) {
        notify(SVC_AUDIO_EVT_PLAYBACK_ERROR, SVC_AUDIO_STATE_IDLE, ret);
        svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_ERROR, NULL, 0);
    }
    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, NULL, 0);
    ESP_LOGI(TAG, "mp3 %s: %u samples", (ret == ESP_OK) ? "done" : "aborted", (unsigned)samples_out);
    return ret;
}

static esp_err_t play_file(const char *path)
{
    if (has_suffix(path, ".mp3")) {
        return play_mp3(path);
    }

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

    bool ok = true;
    while (done < total && !s_stop_req) {
        uint32_t n = total - done;
        if (n > AUDIO_CHUNK / 4) n = AUDIO_CHUNK / 4;

        for (uint32_t i = 0; i < n; i++) {
            int16_t v = (int16_t)(sinf(2.0f * AUDIO_PI * (float)freq * (float)(done + i) / (float)sr) * 10000.0f);
            pcm[2 * i] = v;
            pcm[2 * i + 1] = v;
        }

        if (periph_audio_write(buf, n * 4, 0) != ESP_OK) {
            ok = false;
            break;
        }
        done += n;
    }

    free(buf);
    periph_audio_set_mute(true);

    if (!ok) {
        svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_ERROR, NULL, 0);
    }
    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, NULL, 0);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ------------------------------ 录音（ES7210） ------------------------------ */

/* 写标准 44 字节 PCM WAV 头（固定 16-bit 立体声，采样率 AUDIO_SR_DEFAULT） */
static void wav_write_header(FILE *f, uint32_t data_bytes)
{
    const uint16_t channels = 2;
    const uint16_t bits = 16;
    const uint32_t rate = (uint32_t)AUDIO_SR_DEFAULT;
    const uint32_t byte_rate = rate * channels * bits / 8;
    const uint16_t block_align = (uint16_t)(channels * bits / 8);
    const uint32_t riff_size = 36 + data_bytes;
    const uint32_t fmt_size = 16;
    const uint16_t pcm = 1;

    uint8_t h[44];
    memset(h, 0, sizeof(h));
    memcpy(&h[0], "RIFF", 4);
    memcpy(&h[4], &riff_size, 4);
    memcpy(&h[8], "WAVE", 4);
    memcpy(&h[12], "fmt ", 4);
    memcpy(&h[16], &fmt_size, 4);
    memcpy(&h[20], &pcm, 2);
    memcpy(&h[22], &channels, 2);
    memcpy(&h[24], &rate, 4);
    memcpy(&h[28], &byte_rate, 4);
    memcpy(&h[32], &block_align, 2);
    memcpy(&h[34], &bits, 2);
    memcpy(&h[36], "data", 4);
    memcpy(&h[40], &data_bytes, 4);

    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
}

static esp_err_t record_wav(const char *path, uint32_t max_seconds)
{
    periph_audio_format_t fmt = {
        .sample_rate = AUDIO_SR_DEFAULT, .bit_width = 16, .channels = 2,
    };
    esp_err_t err = periph_audio_set_format(PERIPH_AUDIO_DIR_RECORD, &fmt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "record format failed: %s", esp_err_to_name(err));
        return err;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *buf = malloc(AUDIO_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint8_t placeholder[44] = { 0 };
    fwrite(placeholder, 1, sizeof(placeholder), f);   /* 结束后回填真实长度 */

    s_stop_req = false;
    enter_state(SVC_AUDIO_STATE_RECORDING);
    ESP_LOGI(TAG, "recording %s (max %us)", path, (unsigned)max_seconds);

    const uint32_t limit = (uint32_t)AUDIO_SR_DEFAULT * 2 * 2 * max_seconds;   /* 16-bit 立体声字节数 */
    uint32_t total = 0;
    bool ok = true;

    while (!s_stop_req && total < limit) {
        size_t rd = 0;
        if (periph_audio_read(buf, AUDIO_CHUNK, &rd, 200) != ESP_OK || rd == 0) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, rd, f) != rd) {
            ok = false;
            break;
        }
        total += (uint32_t)rd;
    }

    free(buf);
    wav_write_header(f, total);
    fclose(f);

    enter_state(SVC_AUDIO_STATE_IDLE);
    notify(SVC_AUDIO_EVT_RECORD_FINISHED, SVC_AUDIO_STATE_IDLE, ok ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "record %s: %u bytes", ok ? "done" : "aborted", (unsigned)total);
    return ok ? ESP_OK : ESP_FAIL;
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

        if (msg.record) {
            record_wav(msg.uri, msg.max_seconds);
        } else if (msg.type == SVC_AUDIO_SRC_FILE) {
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
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_audio_deinit(void)
{
    s_ready = false;
    s_stop_req = true;                    /* 让正在播的内容尽快退出 */

    if (s_task != NULL) {
        vTaskDelete(s_task);              /* 任务阻塞在队列上，直接删除 */
        s_task = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    enter_state(SVC_AUDIO_STATE_IDLE);
    ESP_LOGI(TAG, "deinitialized");
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
    if (file_path == NULL || file_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_state == SVC_AUDIO_STATE_RECORDING) return ESP_ERR_INVALID_STATE;

    audio_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.record = true;
    msg.max_seconds = (max_seconds == 0) ? 60 : max_seconds;   /* 0 视为默认 60 秒上限 */
    strlcpy(msg.uri, file_path, sizeof(msg.uri));

    s_stop_req = true;                       /* 打断当前播放 */
    if (xQueueOverwrite(s_queue, &msg) != pdTRUE) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t svc_audio_record_stop(void)
{
    if (s_state != SVC_AUDIO_STATE_RECORDING) return ESP_ERR_INVALID_STATE;

    s_stop_req = true;                       /* 录音循环据此收尾并回填 WAV 头 */
    return ESP_OK;
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
