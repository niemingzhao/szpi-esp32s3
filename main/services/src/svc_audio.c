/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 音频实现（播放 / 录音 / 音量）
 *
 * play_task 串行处理播放 / 录音请求：
 *   WAV（PCM 16-bit，单 / 双声道）→ periph_audio_write
 *   MP3（helix 解码）            → periph_audio_write
 *   HTTP(S) 链接（边下边播 MP3）  → esp_http_client 读 → helix 解码 → periph_audio_write
 *   TONE                        → 正弦波合成
 *   录音（ES7210）               → periph_audio_read 写入 WAV 文件
 *
 * 外部 PCM 流（SVC_AUDIO_SRC_STREAM）不经过本任务：调用方用
 * svc_audio_stream_start / svc_audio_write / svc_audio_stream_stop 直接借用播放通路。
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mp3dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>       /* fsync() / fileno() */

static const char *TAG = "svc.audio";

#define AUDIO_NS            "sys"
#define AUDIO_VOL_KEY       "volume"
#define AUDIO_MUTE_KEY      "muted"
/* 栈必须放内部 RAM（任务会写 NVS / TF 卡，flash 操作期间 cache 关闭，PSRAM 栈会崩），
 * 所以开不大：内部 RAM 要留给相机 DVP 的 7680 字节连续 DMA。链接播放的 TLS 握手因此
 * 另起临时任务，见 play_url()。 */
#define AUDIO_TASK_STACK    4096
#define AUDIO_TASK_PRIO     6
#define AUDIO_TASK_CORE     1
#define AUDIO_CHUNK         4096     /* 单次读取字节数 */
#define AUDIO_REC_READ_RETRY 5       /* 录音读失败的重试次数（每轮 200 ms 超时，约 1 s 余量） */
#define AUDIO_URI_MAX       256
#define AUDIO_SR_DEFAULT    SVC_AUDIO_SR_16K
#define AUDIO_PI            3.14159265f
#define AUDIO_HTTP_TIMEOUT_MS   10000   /* 链接播放：连接 / 单次读超时 */
#define AUDIO_STREAM_WAIT_MS    1000    /* 打开流时等待上一条播放退出 */
#define AUDIO_RECORD_MAX_SEC    3600    /* 单次录音时长上限 */
#define AUDIO_WRITE_TIMEOUT_MS  1000    /* 单次写 I2S 的超时：卡住时尽快收尾，不无限等 */
#define AUDIO_AMP_SETTLE_MS     150     /* 功放启动斜坡：PA_EN 拉高后要等这么久才出声 */

typedef struct {
    svc_audio_src_type_t type;
    char uri[AUDIO_URI_MAX];
    uint16_t tone_freq;
    uint32_t tone_duration_ms;
    bool record;             /* 本次请求是录音（uri 为目标文件，max_seconds 为时长上限） */
    uint32_t max_seconds;
} audio_msg_t;

static QueueHandle_t s_queue = NULL;
static volatile svc_audio_state_t s_state = SVC_AUDIO_STATE_IDLE;
static volatile bool s_stop_req = false;
static volatile bool s_pause_req = false;
static volatile bool s_stream_active = false;   /* 外部 PCM 流已打开（调用方在喂数据） */

static uint8_t s_volume = 80;
static bool s_muted = false;
static bool s_ready = false;

/* 音量落 NVS 的消抖：拖动滑块时每格都会调 svc_audio_set_volume()，
 * 直接写 flash 就是几十次写放大（每次 nvs_commit 还要卡住 LVGL 任务几毫秒）。
 * 这里只记待写值，由音频任务空闲满 1 s 后再写一次（见 audio_task）。
 * 静音切换很少发生，不用消抖、直接写（见 svc_audio_set_mute） */
#define AUDIO_VOL_SAVE_MS   1000
static int s_vol_pending = -1;

static void vol_save_flush(void)
{
    if (s_vol_pending >= 0) {
        svc_settings_set_u8(AUDIO_NS, AUDIO_VOL_KEY, (uint8_t)s_vol_pending);
        s_vol_pending = -1;
    }
}

/* ------------------------------ 状态 ------------------------------ */

static void enter_state(svc_audio_state_t st)
{
    s_state = st;
}

/* 播放前的准备失败（链接打不开 / 非 2xx 等）：发事件、回 IDLE，并返回错误码 */
static esp_err_t playback_fail(const char *what, const char *uri, esp_err_t err)
{
    ESP_LOGE(TAG, "%s failed: %s (%s)", what, (uri != NULL) ? uri : "-", esp_err_to_name(err));
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_ERROR, NULL, 0);
    enter_state(SVC_AUDIO_STATE_IDLE);
    return err;
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

/* MP3 输入来源：本地文件或 HTTP(S) 边下边播 */
typedef struct {
    FILE *f;                          /* 文件来源（HTTP 时为 NULL） */
    esp_http_client_handle_t http;    /* HTTP 来源（文件时为 NULL） */
    bool eof;
} mp3_src_t;

/* 从输入来源读最多 max 字节，返回实际读到的字节数；读完 / 出错都置 eof */
static size_t mp3_src_read(mp3_src_t *s, uint8_t *dst, size_t max)
{
    if (s->http != NULL) {
        int rd = esp_http_client_read(s->http, (char *)dst, (int)max);
        if (rd <= 0) {
            if (rd < 0) ESP_LOGW(TAG, "http read error: %d", rd);
            s->eof = true;
            return 0;
        }
        return (size_t)rd;
    }

    size_t rd = fread(dst, 1, max, s->f);
    if (rd == 0) s->eof = true;
    return rd;
}

/* helix 解码 → 16-bit PCM → periph_audio_write；单声道帧复制成双声道。
 * 文件与 HTTP 链接共用这条通路，label 仅用于日志。 */
static esp_err_t mp3_play_core(mp3_src_t *src, const char *label)
{
    HMP3Decoder dec = MP3InitDecoder();
    uint8_t *in = malloc(MP3_IN_BUF);
    int16_t *out = malloc(MP3_OUT_SAMPLES * sizeof(int16_t));
    if (dec == NULL || in == NULL || out == NULL) {
        ESP_LOGE(TAG, "mp3 buffer alloc failed");
        if (dec != NULL) MP3FreeDecoder(dec);
        free(in);
        free(out);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    uint8_t *p = in;                 /* 当前解析位置 */
    int left = 0;                    /* 缓冲中剩余字节 */
    int cur_rate = 0;
    uint32_t samples_out = 0;

    ESP_LOGI(TAG, "playing mp3 %s", label);
    enter_state(SVC_AUDIO_STATE_PLAYING);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_STARTED, NULL, 0);
    if (!s_muted) periph_audio_set_mute(false);

    while (!s_stop_req) {
        while (s_pause_req && !s_stop_req) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_stop_req) break;

        /* 补数据：把还没解析的部分搬到缓冲开头，再读满 */
        if (left < MP3_REFILL_WATER && !src->eof) {
            if (left > 0) memmove(in, p, (size_t)left);
            size_t rd = mp3_src_read(src, in + left, MP3_IN_BUF - (size_t)left);
            left += (int)rd;
            p = in;
        }
        if (left <= 0) break;                          /* 数据读完 */

        int off = MP3FindSyncWord(p, left);
        if (off < 0) {
            left = 0;                                  /* 没有同步字：整段丢掉继续读 */
            if (src->eof) break;
            continue;
        }
        p += off;
        left -= off;

        int err = MP3Decode(dec, &p, &left, out, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (src->eof) break;
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
                               (size_t)frames * 2 * sizeof(int16_t), AUDIO_WRITE_TIMEOUT_MS) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
        samples_out += (uint32_t)frames;
    }

    free(out);
    free(in);
    MP3FreeDecoder(dec);

    periph_audio_set_mute(true);

    if (ret != ESP_OK) {
        svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_ERROR, NULL, 0);
    }
    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, NULL, 0);
    ESP_LOGI(TAG, "mp3 %s: %u samples", (ret == ESP_OK) ? "done" : "aborted", (unsigned)samples_out);
    return ret;
}

static esp_err_t play_mp3(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    mp3_src_t src = { .f = f };
    esp_err_t ret = mp3_play_core(&src, path);
    fclose(f);
    return ret;
}

/* 链接的 TLS 握手（esp-tls / mbedTLS）栈开销大，而音频任务只有 4 KB 栈，所以握手丢给一个
 * 临时任务（6 KB，用完即退），音频任务只负责后续的流式读取 —— 栈开销回到本地文件那一档。 */
#define AUDIO_HTTP_TASK_STACK 6144

typedef struct {
    const char *url;
    esp_http_client_handle_t client;
    esp_err_t err;
    volatile bool done;
} url_open_job_t;

static void url_open_task(void *arg)
{
    url_open_job_t *job = arg;

    const esp_http_client_config_t cfg = {
        .url = job->url,
        .timeout_ms = AUDIO_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* https 走内置证书 bundle */
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        job->err = ESP_FAIL;
        job->done = true;
        return;
    }

    job->err = esp_http_client_open(c, 0);
    if (job->err != ESP_OK) {
        esp_http_client_cleanup(c);
        job->done = true;
        return;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        job->err = ESP_FAIL;
        job->done = true;
        return;
    }

    const int status = esp_http_client_get_status_code(c);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "http %d: %s", status, job->url);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        job->err = ESP_ERR_INVALID_RESPONSE;
        job->done = true;
        return;
    }

    job->client = c;             /* 交给音频任务继续流式读取 */
    job->err = ESP_OK;
    job->done = true;
}

/* HTTP(S) 链接：临时任务打开连接，音频任务流式读取 → 同一个 MP3 解码通路 */
static esp_err_t play_url(const char *url)
{
    url_open_job_t job = { .url = url, .client = NULL, .err = ESP_OK, .done = false };

    if (xTaskCreate(url_open_task, "audio.http", AUDIO_HTTP_TASK_STACK, &job,
                    AUDIO_TASK_PRIO, NULL) != pdPASS) {
        return playback_fail("http task", url, ESP_ERR_NO_MEM);
    }

    while (!job.done) vTaskDelay(pdMS_TO_TICKS(10));

    if (job.client == NULL) {
        return playback_fail("http open", url, job.err);
    }

    ESP_LOGI(TAG, "streaming %s", url);
    mp3_src_t src = { .http = job.client };
    const esp_err_t err = mp3_play_core(&src, url);

    esp_http_client_close(job.client);
    esp_http_client_cleanup(job.client);
    return err;
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

        if (periph_audio_write(buf, out_len, AUDIO_WRITE_TIMEOUT_MS) != ESP_OK) {
            ok = false;
            break;
        }
    }

    free(buf);
    fclose(f);

    periph_audio_set_mute(true);                     /* 关闭功放 */

    if (!ok) {
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
        .sample_rate = (periph_audio_sample_rate_t)AUDIO_SR_DEFAULT, .bit_width = 16, .channels = 2,
    };
    esp_err_t err = periph_audio_set_format(PERIPH_AUDIO_DIR_PLAY, &fmt);
    if (err != ESP_OK) return err;

    uint8_t *buf = malloc(AUDIO_CHUNK * 2);
    if (buf == NULL) return ESP_ERR_NO_MEM;

    if (!s_muted) periph_audio_set_mute(false);

    /* 功放（NS4150B）从 PA_EN 拉高到真正出声有几百毫秒的启动斜坡：提示音很短，不等这段
     * 就会被斜坡整段吃掉（表现是"响了但听不到"）。等够再写 PCM，和开机提示音一个做法。 */
    vTaskDelay(pdMS_TO_TICKS(AUDIO_AMP_SETTLE_MS));

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

        if (periph_audio_write(buf, n * 4, AUDIO_WRITE_TIMEOUT_MS) != ESP_OK) {
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

/* ------------------------------ 录音环形缓冲 ------------------------------
 *
 * 录音要同时干两件事：读 I2S（不能停，否则 RX 的几十毫秒 DMA 环立刻溢出）和写 FATFS
 * （跨簇时更新 FAT / 目录项会卡上百毫秒）。放在同一个循环里必然二选一 —— 之前是写卡顿
 * 把读拖停，表现为录音里每 1~3 s 丢 0.2~0.4 s 的空档。
 *
 * 现在拆开：audio_task 只读 I2S 往环形缓冲里塞（塞不下就丢这一块，绝不阻塞），
 * 写卡由 rec_write_task 从缓冲里取。缓冲放 PSRAM（64 KB ≈ 1 s），写卡顿由它吸收。
 * 单生产者单消费者，head / tail 各自只由一方写，用 volatile + 无符号回绕就够，不用锁。
 */

#define AUDIO_REC_RING_SIZE   (64 * 1024)   /* 2 的幂 */
#define AUDIO_REC_WRITE_STACK 4096
#define AUDIO_REC_SYNC_BYTES  (64 * 1024)   /* 每写满这么多字节 f_sync 一次：文件大小及时可见 */

/* 写卡任务挂到另一个核心上：SDMMC / FATFS 写卡时会长时间占住所在核心，
 * 跟读 I2S 放同一个核心会把读侧饿死（实测 RX 每 ~250 ms 溢出一次、每次只收到几百字节）。
 * 换核心后写卡再慢也只影响它自己，读侧照常按 60 ms 的节拍取数据 */
#define AUDIO_REC_WRITE_CORE  ((AUDIO_TASK_CORE == 0) ? 1 : 0)

typedef struct {
    uint8_t *buf;
    size_t cap;                 /* 2 的幂，取模用 & (cap - 1) */
    volatile size_t head;       /* 生产者写入位置（自由回绕） */
    volatile size_t tail;       /* 消费者读取位置 */
    volatile bool done;         /* 生产者结束 */
    volatile bool failed;       /* 写失败：让生产者尽快收手 */
    FILE *fp;
    uint32_t written;
    uint32_t synced;                        /* 上次 f_sync 时的 written */
    size_t dropped;
    TaskHandle_t task;
} rec_ring_t;

/* 读缓冲占用（自己写的那一头不必加屏障，另一头的值用 acquire 保证它后面的数据也可见） */
static size_t rec_ring_used(const rec_ring_t *r)
{
    const size_t head = r->head;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return (head - r->tail) & (r->cap - 1);
}

/* 放进环形缓冲；放不下就整块丢掉（不阻塞：读侧一停，RX 就会溢出） */
static void rec_ring_push(rec_ring_t *r, const uint8_t *data, size_t len)
{
    if (len == 0) return;
    if (len + rec_ring_used(r) >= r->cap) {
        r->dropped += len;
        return;
    }

    const size_t idx = r->head & (r->cap - 1);
    const size_t first = (len < r->cap - idx) ? len : (r->cap - idx);
    memcpy(r->buf + idx, data, first);
    if (len > first) memcpy(r->buf, data + first, len - first);

    /* 两个任务可能跑在不同核心上：数据先落地，再更新 head（release） */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->head = (r->head + len) & (r->cap - 1);
}

/* 写卡任务：只负责从缓冲取数据写文件，卡顿都卡在这里；结束时自己回填 WAV 头并关文件
 * （文件由它独占，记录侧不再碰，避免两边抢） */
static void rec_write_task(void *arg)
{
    rec_ring_t *r = (rec_ring_t *)arg;

    for (;;) {
        const size_t used = rec_ring_used(r);

        if (used > 0) {
            const size_t idx = r->tail & (r->cap - 1);
            const size_t first = (used < r->cap - idx) ? used : (r->cap - idx);

            if (fwrite(r->buf + idx, 1, first, r->fp) != first) {
                ESP_LOGE(TAG, "record write failed at %u bytes", (unsigned)r->written);
                r->failed = true;
                break;
            }
            r->written += (uint32_t)first;

            /* 定期把数据和目录项刷到卡上：FATFS 只在关文件时才更新目录项里的长度，
             * 录音过程中去看文件一直是 0 字节。FILE* 上没有 f_sync()（那是 FatFs 私有
             * API，要 FIL*），走 VFS 的 fsync(fileno()) 即可 —— vfs_fat 里就是 f_sync */
            if (r->written - r->synced >= AUDIO_REC_SYNC_BYTES) {
                r->synced = r->written;
                const int fd = fileno(r->fp);
                if (fd < 0 || fsync(fd) != 0) {
                    ESP_LOGW(TAG, "record fsync failed");
                }
            }

            /* 这段数据已经写完，先让出空间再更新 tail（release） */
            __atomic_thread_fence(__ATOMIC_RELEASE);
            r->tail = (r->tail + first) & (r->cap - 1);
            continue;                       /* 绕回来那段下一轮再写 */
        }

        if (r->done) break;                 /* 生产者结束且已经排空 */
        vTaskDelay(pdMS_TO_TICKS(10));      /* 等新数据 */
    }

    if (!r->failed) wav_write_header(r->fp, r->written);
    fclose(r->fp);
    r->fp = NULL;
    r->task = NULL;                         /* 记录侧据此判断"可以收尾了" */
    vTaskDelete(NULL);
}

/* 环形缓冲是单实例（同一时刻只有一段录音），放静态区：写卡任务拿着它的指针，
 * 不能用栈上的临时结构 */
static rec_ring_t s_rec_ring;

static esp_err_t record_wav(const char *path, uint32_t max_seconds)
{
    periph_audio_format_t fmt = {
        .sample_rate = (periph_audio_sample_rate_t)AUDIO_SR_DEFAULT, .bit_width = 16, .channels = 2,
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
    rec_ring_t *ring = &s_rec_ring;

    if (ring->task != NULL) {
        /* 上一段的写卡任务还没退（极少见）：等它收完，别两段录音抢缓冲 */
        ESP_LOGW(TAG, "previous record writer still running, waiting");
        while (ring->task != NULL) vTaskDelay(pdMS_TO_TICKS(10));
    }

    memset(ring, 0, sizeof(*ring));
    ring->buf = malloc(AUDIO_REC_RING_SIZE);
    ring->cap = AUDIO_REC_RING_SIZE;
    ring->fp = f;

    if (buf == NULL || ring->buf == NULL) {
        free(buf);
        free(ring->buf);
        ring->buf = NULL;
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint8_t placeholder[44] = { 0 };
    fwrite(placeholder, 1, sizeof(placeholder), f);   /* 结束后回填真实长度 */

    /* 写卡任务挂到另一个核心、优先级低一档：写卡再慢也不占读侧的核心，
     * 读侧照常按 I2S 的节拍取数据往缓冲里塞 */
    if (xTaskCreatePinnedToCore(rec_write_task, "audio.wr", AUDIO_REC_WRITE_STACK, ring,
                                AUDIO_TASK_PRIO - 1, &ring->task, AUDIO_REC_WRITE_CORE) != pdPASS) {
        ESP_LOGE(TAG, "record writer task failed");
        free(buf);
        free(ring->buf);
        ring->buf = NULL;
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    s_stop_req = false;
    enter_state(SVC_AUDIO_STATE_RECORDING);
    svc_event_bus_publish(SVC_EVENT_AUDIO_RECORD_STARTED, NULL, 0);
    ESP_LOGI(TAG, "recording %s (max %us)", path, (unsigned)max_seconds);

    const uint32_t limit = (uint32_t)AUDIO_SR_DEFAULT * 2 * 2 * max_seconds;   /* 16-bit 立体声字节数 */
    uint32_t total = 0;
    uint32_t fails = 0;
    uint32_t partials = 0;                  /* 驱动报错但拿到了数据的次数（看看还会不会溢出） */
    bool ok = true;

    while (!s_stop_req && !ring->failed && total < limit) {
        size_t rd = 0;
        const esp_err_t rerr = periph_audio_read(buf, AUDIO_CHUNK, &rd, 200);

        /* 读到多少算多少：驱动报超时 / 溢出时通常也已经塞了一部分数据进来，
         * 这部分是真音频，丢掉就是一段空档（听感就是"滴滴"的断点） */
        if (rd > 0) {
            fails = 0;
            if (rerr != ESP_OK) partials++;
            rec_ring_push(ring, buf, rd);
            total += (uint32_t)rd;
            continue;
        }

        /* 一个字节都没有：如果这时已经按了停止，那是正常收尾，不算读失败 */
        if (s_stop_req) break;

        /* 真卡住：重开 RX 清掉溢出状态再试，连续几次读不到才认输 */
        if (++fails <= AUDIO_REC_READ_RETRY) {
            ESP_LOGW(TAG, "record read got nothing (%s), retry %u/%u",
                     esp_err_to_name(rerr), (unsigned)fails, (unsigned)AUDIO_REC_READ_RETRY);
            periph_audio_rx_restart();
            continue;
        }

        ESP_LOGE(TAG, "record read failed: %s, gave up after %u bytes",
                 esp_err_to_name(rerr), (unsigned)total);
        ok = false;
        break;
    }

    /* 收尾：让写卡任务排空缓冲、回填 WAV 头并关文件。这里必须等它退出 ——
     * 缓冲和文件都归它管，不等就释放会踩到它正在读的内存 */
    ring->done = true;
    uint32_t waited = 0;
    while (ring->task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++waited % 100 == 0) ESP_LOGW(TAG, "record writer draining (%u ms)", (unsigned)(waited * 10));
    }

    if (ring->failed || ring->dropped > 0) {
        ok = false;                          /* 写失败 / 丢过数据都算"没录全" */
    }

    const uint32_t written = ring->written;
    const size_t dropped = ring->dropped;
    free(buf);
    free(ring->buf);
    ring->buf = NULL;

    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_RECORD_FINISHED, NULL, 0);
    ESP_LOGI(TAG, "record %s: %u bytes read, %u written, %u dropped, %u partial",
             ok ? "done" : "aborted", (unsigned)total, (unsigned)written,
             (unsigned)dropped, (unsigned)partials);
    return ok ? ESP_OK : ESP_FAIL;
}

/* -------------------------------- play_task -------------------------------- */

static void audio_task(void *arg)
{
    (void)arg;
    audio_msg_t msg;

    while (true) {
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(AUDIO_VOL_SAVE_MS)) != pdTRUE) {
            vol_save_flush();             /* 空闲满 1 s：把待写的音量落 NVS */
            continue;
        }

        vol_save_flush();                 /* 有命令进来也先落一次，别等空闲 */

        s_stop_req = false;
        s_pause_req = false;

        if (msg.record) {
            record_wav(msg.uri, msg.max_seconds);
        } else if (msg.type == SVC_AUDIO_SRC_FILE) {
            play_file(msg.uri);
        } else if (msg.type == SVC_AUDIO_SRC_URL) {
            play_url(msg.uri);
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

    /* 静音状态与音量一样持久化（重启后仍保持静音） */
    uint8_t muted = 0;
    svc_settings_get_u8(AUDIO_NS, AUDIO_MUTE_KEY, &muted, 0);
    s_muted = (muted != 0);
    periph_audio_set_mute(s_muted);

    s_queue = xQueueCreate(1, sizeof(audio_msg_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "svc_audio", AUDIO_TASK_STACK, NULL,
                                            AUDIO_TASK_PRIO, NULL, AUDIO_TASK_CORE);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "initialized (volume=%u%%, mute=%d)", (unsigned)s_volume, (int)s_muted);
    return ESP_OK;
}

esp_err_t svc_audio_play(const svc_audio_source_t *src)
{
    if (src == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    if (src->type == SVC_AUDIO_SRC_STREAM) {
        /* 外部 PCM 流：svc_audio_source_t 不带格式，用默认 16 kHz 双声道打开，
         * 需要别的采样率请直接调 svc_audio_stream_start(&fmt)。 */
        svc_audio_format_t fmt = {
            .sample_rate = AUDIO_SR_DEFAULT, .bit_width = 16, .channels = 2,
        };
        return svc_audio_stream_start(&fmt);
    }

    if (src->type != SVC_AUDIO_SRC_FILE && src->type != SVC_AUDIO_SRC_URL &&
        src->type != SVC_AUDIO_SRC_TONE) {
        ESP_LOGW(TAG, "source type %d not supported", src->type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((src->type == SVC_AUDIO_SRC_FILE || src->type == SVC_AUDIO_SRC_URL) && src->uri == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 外部 PCM 流正在喂数据时先收尾，避免两条通路同时写 I2S */
    if (s_stream_active) svc_audio_stream_stop();

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
    if (s_stream_active) svc_audio_stream_stop();   /* 外部流：直接收尾回 IDLE */
    return ESP_OK;
}

/* ---------------------------- 外部 PCM 流（STREAM） ---------------------------- */

esp_err_t svc_audio_stream_start(const svc_audio_format_t *fmt)
{
    if (fmt == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_stream_active) return ESP_ERR_INVALID_STATE;
    if (fmt->bit_width != 16 || fmt->channels == 0 || fmt->channels > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state == SVC_AUDIO_STATE_RECORDING) return ESP_ERR_INVALID_STATE;

    /* 打断仍在跑的队列播放，等它让出播放通路（链接边下边播可能等不到，这时返回忙） */
    s_stop_req = true;
    for (uint32_t waited = 0; s_state != SVC_AUDIO_STATE_IDLE && waited < AUDIO_STREAM_WAIT_MS;
         waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_state != SVC_AUDIO_STATE_IDLE) {
        ESP_LOGW(TAG, "stream start: playback still busy");
        return ESP_ERR_INVALID_STATE;
    }

    /* Services 的格式转成 Peripherals 的格式（两层各有一套类型，避免头文件互相暴露） */
    const periph_audio_format_t pfmt = {
        .sample_rate = (periph_audio_sample_rate_t)fmt->sample_rate,
        .bit_width = fmt->bit_width,
        .channels = fmt->channels,
    };
    esp_err_t err = periph_audio_set_format(PERIPH_AUDIO_DIR_PLAY, &pfmt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream format failed: %s", esp_err_to_name(err));
        return err;
    }

    s_stop_req = false;
    s_pause_req = false;
    s_stream_active = true;
    if (!s_muted) periph_audio_set_mute(false);
    enter_state(SVC_AUDIO_STATE_PLAYING);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_STARTED, NULL, 0);
    ESP_LOGI(TAG, "stream start: %u Hz, %u ch", (unsigned)fmt->sample_rate, fmt->channels);
    return ESP_OK;
}

esp_err_t svc_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_stream_active) return ESP_ERR_INVALID_STATE;
    if (s_stop_req) return ESP_ERR_INVALID_STATE;   /* 已被 stop / 新播放请求打断 */

    while (s_pause_req && !s_stop_req) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_stop_req) return ESP_ERR_INVALID_STATE;

    return periph_audio_write(data, len, timeout_ms);
}

esp_err_t svc_audio_stream_stop(void)
{
    if (!s_stream_active) return ESP_ERR_INVALID_STATE;

    s_stream_active = false;
    s_stop_req = true;                       /* 让可能仍在写的调用方尽快退出 */
    periph_audio_set_mute(true);
    enter_state(SVC_AUDIO_STATE_IDLE);
    svc_event_bus_publish(SVC_EVENT_AUDIO_PLAYBACK_FINISHED, NULL, 0);
    ESP_LOGI(TAG, "stream stop");
    return ESP_OK;
}

esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds)
{
    if (file_path == NULL || file_path[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_state == SVC_AUDIO_STATE_RECORDING) return ESP_ERR_INVALID_STATE;
    if (s_stream_active) return ESP_ERR_INVALID_STATE;   /* 外部 PCM 流占用播放通路 */

    audio_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.record = true;
    /* 0 视为默认 60 s；再封一个上限，避免 record_wav 里按秒算字节数溢出 */
    if (max_seconds == 0) max_seconds = 60;
    if (max_seconds > AUDIO_RECORD_MAX_SEC) max_seconds = AUDIO_RECORD_MAX_SEC;
    msg.max_seconds = max_seconds;
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

        /* 只记待写值，由音频任务停稳后落 NVS（拖动时不做 flash 写，见 AUDIO_VOL_SAVE_MS） */
        s_vol_pending = (int)percent;
    }
    return ESP_OK;
}

uint8_t svc_audio_get_volume(void)
{
    return s_volume;
}

esp_err_t svc_audio_set_mute(bool mute)
{
    const bool changed = (s_muted != mute);

    s_muted = mute;
    if (s_ready) periph_audio_set_mute(mute);

    /* 静音切换很少发生，直接落 NVS（不跟随音量的消抖），免得刚静音就断电丢掉；
     * 状态没变就不写，省一次 flash */
    if (changed && s_ready) {
        svc_settings_set_u8(AUDIO_NS, AUDIO_MUTE_KEY, mute ? 1 : 0);
    }
    return ESP_OK;
}

bool svc_audio_get_mute(void)
{
    return s_muted;
}

svc_audio_state_t svc_audio_get_state(void)
{
    return s_state;
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
