/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 系统信息 / 崩溃记录 / 性能监控 实现
 *
 * 崩溃记录：崩溃时把现场（复位原因 / 任务名 / 调用栈 PC / SP）写进 RTC 不初始化内存，
 * 下次启动读出来写 NVS（只保留最近几条）并打进串口日志。不用 espcoredump：它要额外的
 * coredump 分区，而本工程的分区表不加新分区；这里改为链接期 wrap IDF 的
 * esp_panic_handler 抓现场，见 __wrap_esp_panic_handler()。
 *
 * 任务快照：用 uxTaskGetSystemState() 给出每个任务的 CPU 占用与栈余量，需要打开
 * CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS。
 */

#include "svc_common.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "svc.sysinfo";

/* ------------------------------ 最近日志环 ------------------------------ */

/* 无锁字节环：写者（日志钩子，可能在任意任务 / 中断上下文）只推进 s_log_head，
 * 读者可能看到半行，用于调试控制台足够。2 KB 放在 BSS，不占堆。 */
#define SYSINFO_LOG_RING_SIZE   2048u
#define SYSINFO_LOG_LINE_MAX    256u     /* 单行最多抄这么多；超长截断（Bluedroid 的日志偏长） */

static char s_log_ring[SYSINFO_LOG_RING_SIZE];
static volatile uint32_t s_log_head = 0;
static vprintf_like_t s_prev_vprintf = NULL;

static void sysinfo_log_ring_push(const char *s, size_t len)
{
    uint32_t head = s_log_head;
    for (size_t i = 0; i < len; i++) {
        s_log_ring[head & (SYSINFO_LOG_RING_SIZE - 1)] = s[i];
        head++;
    }
    s_log_head = head;
}

/* 日志钩子的重入保护。
 *
 * 日志输出路径内部可能再触发日志：例如 uart_write_bytes() 开头的
 * ESP_RETURN_ON_FALSE 条件不满足时会打 "uart driver error"，而那条日志又要走输出 →
 * 又回到本函数 → 无限递归，最终爆栈并写坏 FreeRTOS 对象（现场是
 * assert xQueueSemaphoreTake (pxQueue->uxItemSize == 0) + 满屏重复回溯）。
 *
 * 递归一定发生在同一个任务里，所以用"当前任务是否已经在本函数中"判定重入：
 * 重入时直接返回 0（内层那条属于输出路径自己的日志，丢弃不计入日志环）。
 */
static TaskHandle_t s_log_owner = NULL;

/* 抄日志要用 vsnprintf，它的栈开销（几百字节到 ~1 KB，本工程是 -Og，帧比 -O2 大）算在
 * 调用者头上，所以剩余栈不够时只能跳过抄录（只保留原有串口输出）。阈值留足余量。 */
#define SYSINFO_LOG_MIN_STACK  1024u

static volatile uint32_t s_log_skipped = 0;

/* 这里必须用当前剩余栈判断，不能用 uxTaskGetStackHighWaterMark()：那个是"历史最低
 * 水位"，任务只要在初始化阶段深压过一次（几乎每个 IDF / Bluedroid 任务都会），水位就
 * 永远偏低，之后它所有日志都会被丢掉 —— 表现就是「系统日志」里只有少数几个"一直很浅"
 * 的任务的行，跟串口对不上。
 *
 * 栈向下生长，xTaskGetStackStart() 给的是栈底（最低地址），当前栈指针到栈底之间就是
 * 还没用到的部分。 */
static size_t sysinfo_log_free_stack(void)
{
    StackType_t *base = xTaskGetStackStart(NULL);      /* NULL = 当前任务 */
    if (base == NULL) return 0;

    uint8_t probe;                                     /* 取当前栈指针 */
    const uintptr_t sp = (uintptr_t)&probe;
    const uintptr_t lo = (uintptr_t)base;

    return (sp > lo) ? (size_t)(sp - lo) : 0;
}

static int sysinfo_log_vprintf(const char *fmt, va_list args)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (self != NULL && self == s_log_owner) {
        return 0;
    }

    TaskHandle_t prev_owner = s_log_owner;
    s_log_owner = self;

    /* 中断上下文没有"当前任务"（self == NULL），也别去碰栈指针 */
    if (self != NULL && sysinfo_log_free_stack() >= SYSINFO_LOG_MIN_STACK) {
        /* 行缓冲放静态区：不占调用者栈 */
        static char line[SYSINFO_LOG_LINE_MAX];
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        va_end(copy);

        if (n > 0) {
            size_t len = ((size_t)n < sizeof(line) - 1) ? (size_t)n : sizeof(line) - 1;
            if ((size_t)n >= sizeof(line) - 1) {
                line[len - 1] = '\n';       /* 被截断的行也要补换行，别和下一条粘在一起 */
            }
            sysinfo_log_ring_push(line, len);
        }
    } else if (self != NULL) {
        /* 剩余栈确实不够，这一行只在串口可见。攒够一批在串口提一句，方便定位是哪个任务 */
        if ((++s_log_skipped & 0x3fu) == 1u) {
            ESP_LOGE(TAG, "log ring: %s 剩余栈不足，已跳过 %u 行（只在串口显示）",
                     pcTaskGetName(NULL), (unsigned)s_log_skipped);
        }
    }

    int ret = (s_prev_vprintf != NULL) ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    s_log_owner = prev_owner;
    return ret;
}

/* ------------------------------ 崩溃记录 ------------------------------ */

#define SYSINFO_NS                 "sys"
#define SYSINFO_KEY_CRASH          "crash_log"
#define SYSINFO_CRASH_MAGIC        0x535a5043u   /* "SZPC" */
#define SYSINFO_CRASH_PC_MAX       8
#define SYSINFO_CRASH_ENTRY_MAX    128
#define SYSINFO_CRASH_BLOB_MAX     640
#define SYSINFO_CRASH_TMP_MAX      (SYSINFO_CRASH_BLOB_MAX + SYSINFO_CRASH_ENTRY_MAX + 8)

typedef struct {
    uint32_t magic;
    uint32_t uptime_ms;
    uint32_t sp;
    char task[16];
    uint8_t pc_count;
    uint8_t reserved[3];
    uint32_t pc[SYSINFO_CRASH_PC_MAX];
} sysinfo_crash_rtc_t;

/* RTC 不初始化内存：软复位 / panic 后仍保留，但不经过 flash，崩溃上下文里可安全写入 */
RTC_NOINIT_ATTR static sysinfo_crash_rtc_t s_crash_rtc;

/* 崩溃上下文里执行：不能调用 flash 里的函数（libc），字符串自己搬 */
static void IRAM_ATTR sysinfo_capture_crash(void)
{
    /* 顺序很重要：先写"一定安全"的字段，再置 magic，最后补调用栈。
     * 这样万一后面某步出错，也只会丢 PC 列表，不会留下半条假记录。 */
    s_crash_rtc.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_crash_rtc.sp = 0;
    s_crash_rtc.pc_count = 0;

    const char *name = pcTaskGetName(NULL);
    uint8_t k = 0;
    if (name != NULL) {
        while (k < (uint8_t)(sizeof(s_crash_rtc.task) - 1) && name[k] != '\0') {
            s_crash_rtc.task[k] = name[k];
            k++;
        }
    }
    s_crash_rtc.task[k] = '\0';

    s_crash_rtc.magic = SYSINFO_CRASH_MAGIC;   /* 记录到此已可用 */

    /* 补调用栈：依赖栈内容，失败也只丢 PC 列表 */
    esp_backtrace_frame_t frame = {0};
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    s_crash_rtc.sp = frame.sp;

    uint8_t count = 0;
    for (uint8_t i = 0; i < SYSINFO_CRASH_PC_MAX; i++) {
        s_crash_rtc.pc[i] = frame.pc;
        count = (uint8_t)(i + 1);
        if (!esp_backtrace_get_next_frame(&frame)) break;
    }
    s_crash_rtc.pc_count = count;
}

void __real_esp_panic_handler(void *info);
void __wrap_esp_panic_handler(void *info);

void IRAM_ATTR __wrap_esp_panic_handler(void *info)
{
    sysinfo_capture_crash();
    __real_esp_panic_handler(info);
}

/* 追加一条记录到 NVS（只保留最近 SVC_SYSINFO_CRASH_KEEP 条，'\n' 分隔） */
static void sysinfo_record_crash(const char *entry)
{
    char buf[SYSINFO_CRASH_BLOB_MAX];
    size_t len = sizeof(buf);
    if (svc_settings_get_blob(SYSINFO_NS, SYSINFO_KEY_CRASH, buf, &len) != ESP_OK) {
        len = 0;
    }
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    buf[len] = '\0';

    char tmp[SYSINFO_CRASH_TMP_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s%s\n", buf, entry);
    if (n < 0) return;
    size_t used = ((size_t)n < sizeof(tmp)) ? (size_t)n : sizeof(tmp) - 1;

    /* 超出容量时丢掉最旧的几行 */
    while (used > SYSINFO_CRASH_BLOB_MAX - 1) {
        char *nl = strchr(tmp, '\n');
        if (nl == NULL) {
            used = SYSINFO_CRASH_BLOB_MAX - 1;
            tmp[used] = '\0';
            break;
        }
        size_t drop = (size_t)(nl + 1 - tmp);
        memmove(tmp, nl + 1, used - drop + 1);
        used -= drop;
    }
    if (used == 0) {
        used = 1;
        tmp[0] = '\0';
    }

    if (svc_settings_set_blob(SYSINFO_NS, SYSINFO_KEY_CRASH, tmp, used) != ESP_OK) {
        ESP_LOGW(TAG, "save crash log failed");
    }
}

static void sysinfo_consume_crash(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool abnormal = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                     rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);
    bool detailed = (s_crash_rtc.magic == SYSINFO_CRASH_MAGIC);

    if (!abnormal && !detailed) return;

    char entry[SYSINFO_CRASH_ENTRY_MAX];
    int n;
    if (detailed) {
        n = snprintf(entry, sizeof(entry), "reset=%s uptime=%us task=%s sp=%08x pc=",
                     svc_sysinfo_reset_reason_str(),
                     (unsigned)(s_crash_rtc.uptime_ms / 1000),
                     s_crash_rtc.task,
                     (unsigned)s_crash_rtc.sp);
    } else {
        n = snprintf(entry, sizeof(entry), "reset=%s uptime=%us (no detail)",
                     svc_sysinfo_reset_reason_str(),
                     (unsigned)(esp_timer_get_time() / 1000000));
    }
    if (n < 0) n = 0;
    size_t used = ((size_t)n < sizeof(entry)) ? (size_t)n : sizeof(entry) - 1;

    if (detailed) {
        for (uint8_t i = 0; i < s_crash_rtc.pc_count && used + 12 < sizeof(entry); i++) {
            int m = snprintf(entry + used, sizeof(entry) - used, "%08x ", (unsigned)s_crash_rtc.pc[i]);
            if (m < 0) break;
            used += ((size_t)m < sizeof(entry) - used) ? (size_t)m : sizeof(entry) - used - 1;
        }
    }

    s_crash_rtc.magic = 0;   /* 消费掉，避免下次启动重复记录 */

    ESP_LOGE(TAG, "last crash: %s", entry);   /* 验收：串口可读到 crash 信息 */
    sysinfo_record_crash(entry);
}

/* ------------------------------ 任务统计 ------------------------------ */

#define SYSINFO_TASK_MAX 32

static TaskStatus_t s_task_stat[SYSINFO_TASK_MAX];
static TaskHandle_t s_task_prev_handle[SYSINFO_TASK_MAX];
static uint32_t s_task_prev_run[SYSINFO_TASK_MAX];
static UBaseType_t s_task_prev_count = 0;
static uint32_t s_task_prev_total = 0;
static bool s_task_prev_valid = false;
/* 总体 CPU 占用单独一份基准：与任务快照互不干扰，两个接口各自采样 */
static TaskHandle_t s_cpu_prev_handle[SYSINFO_TASK_MAX];
static uint32_t s_cpu_prev_run[SYSINFO_TASK_MAX];
static UBaseType_t s_cpu_prev_count = 0;
static uint32_t s_cpu_prev_total = 0;
static bool s_cpu_prev_valid = false;
static SemaphoreHandle_t s_mux = NULL;

static bool sysinfo_is_idle(const char *name)
{
    return (name != NULL) && (strncmp(name, "IDLE", 4) == 0);
}

/* ---------------------------------- API ---------------------------------- */

esp_err_t svc_sysinfo_log_capture_start(void)
{
    /* 越早调用日志环越完整：放在 main 的第一句，开机过程中各层的日志才抄得进来。
     * 只是装一个 vprintf 钩子，不依赖 NVS / 信号量，重复调用安全 */
    if (s_prev_vprintf == NULL) {
        s_prev_vprintf = esp_log_set_vprintf(sysinfo_log_vprintf);
    }
    return ESP_OK;
}

esp_err_t svc_sysinfo_init(void)
{
    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == NULL) return ESP_ERR_NO_MEM;
    }

    svc_sysinfo_log_capture_start();    /* 一般 main 里已经调过，这里兜底 */
    sysinfo_consume_crash();

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

static const char *sysinfo_chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return "unknown";
    }
}

const char *svc_sysinfo_reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

esp_err_t svc_sysinfo_get(svc_sysinfo_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    out->idf_version = esp_get_idf_version();
    out->reset_reason = svc_sysinfo_reset_reason_str();
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->chip_cores = (uint8_t)ci.cores;
    out->chip_revision = (uint8_t)ci.revision;

    const esp_app_desc_t *app = esp_app_get_description();
    out->project_name = (app != NULL) ? app->project_name : "szpi-os";
    out->app_version = (app != NULL) ? app->version : "unknown";
    out->build_date = (app != NULL) ? app->date : "";
    out->build_time = (app != NULL) ? app->time : "";
    out->chip_model = sysinfo_chip_model_str(ci.model);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out->mac, sizeof(out->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        out->mac[0] = '\0';
    }

    uint32_t flash_size = 0;
    out->flash_size = (esp_flash_get_size(NULL, &flash_size) == ESP_OK) ? flash_size : 0;
    return ESP_OK;
}

esp_err_t svc_sysinfo_get_recent_logs(char *buf, size_t len)
{
    if (buf == NULL || len < 2) return ESP_ERR_INVALID_ARG;

    uint32_t head = s_log_head;
    uint32_t avail = (head < SYSINFO_LOG_RING_SIZE) ? head : SYSINFO_LOG_RING_SIZE;
    size_t n = (avail < (len - 1)) ? (size_t)avail : (len - 1);

    for (size_t i = 0; i < n; i++) {
        buf[i] = s_log_ring[(head - n + i) & (SYSINFO_LOG_RING_SIZE - 1)];
    }
    buf[n] = '\0';

    return (n > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t svc_sysinfo_get_crash_log(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    size_t used = len;
    esp_err_t err = svc_settings_get_blob(SYSINFO_NS, SYSINFO_KEY_CRASH, buf, &used);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    if (used >= len) used = len - 1;
    buf[used] = '\0';

    /* 清除记录留下的是一个空 blob：也按"没有记录"处理 */
    if (used == 0 || buf[0] == '\0') {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t svc_sysinfo_clear_crash_log(void)
{
    const char empty = '\0';
    return svc_settings_set_blob(SYSINFO_NS, SYSINFO_KEY_CRASH, &empty, 1);
}

esp_err_t svc_sysinfo_get_tasks(svc_sysinfo_task_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || max == 0) return ESP_ERR_INVALID_ARG;
    if (s_mux == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_task_stat, SYSINFO_TASK_MAX, &total);
    if (n == 0) {
        xSemaphoreGive(s_mux);
        return ESP_FAIL;
    }

    uint32_t d_total = (s_task_prev_valid && total > s_task_prev_total) ? (total - s_task_prev_total) : 0;

    size_t m = 0;
    for (UBaseType_t i = 0; i < n && m < max; i++) {
        uint32_t d_run = 0;
        if (d_total > 0) {
            for (UBaseType_t k = 0; k < s_task_prev_count; k++) {
                if (s_task_prev_handle[k] == s_task_stat[i].xHandle) {
                    uint32_t cur = (uint32_t)s_task_stat[i].ulRunTimeCounter;
                    if (cur >= s_task_prev_run[k]) d_run = cur - s_task_prev_run[k];
                    break;
                }
            }
        }

        memset(&out[m], 0, sizeof(out[m]));
        if (s_task_stat[i].pcTaskName != NULL) {
            strncpy(out[m].name, s_task_stat[i].pcTaskName, sizeof(out[m].name) - 1);
        }
        out[m].cpu_percent = (uint8_t)((d_total > 0) ? (uint32_t)((uint64_t)d_run * 100u / d_total) : 0u);
        out[m].stack_free = (uint32_t)s_task_stat[i].usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
        out[m].core = (s_task_stat[i].xCoreID == tskNO_AFFINITY) ? -1 : (int8_t)s_task_stat[i].xCoreID;
        m++;
    }

    /* 按 CPU 占用降序（任务不多，直接插入排序） */
    for (size_t i = 1; i < m; i++) {
        svc_sysinfo_task_t key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].cpu_percent < key.cpu_percent) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }

    /* 存快照，供下次算差值 */
    for (UBaseType_t i = 0; i < n && i < SYSINFO_TASK_MAX; i++) {
        s_task_prev_handle[i] = s_task_stat[i].xHandle;
        s_task_prev_run[i] = (uint32_t)s_task_stat[i].ulRunTimeCounter;
    }
    s_task_prev_count = (n < SYSINFO_TASK_MAX) ? n : SYSINFO_TASK_MAX;
    s_task_prev_total = total;
    s_task_prev_valid = true;

    *count = m;
    xSemaphoreGive(s_mux);
    return ESP_OK;
}

esp_err_t svc_sysinfo_get_cpu_usage(uint8_t *percent)
{
    if (percent == NULL) return ESP_ERR_INVALID_ARG;
    *percent = 0;
    if (s_mux == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_task_stat, SYSINFO_TASK_MAX, &total);
    if (n == 0) {
        xSemaphoreGive(s_mux);
        return ESP_FAIL;
    }

    const uint32_t d_total =
        (s_cpu_prev_valid && total > s_cpu_prev_total) ? (total - s_cpu_prev_total) : 0;

    uint32_t busy_run = 0;
    if (d_total > 0) {
        for (UBaseType_t i = 0; i < n; i++) {
            if (sysinfo_is_idle(s_task_stat[i].pcTaskName)) continue;
            for (UBaseType_t k = 0; k < s_cpu_prev_count; k++) {
                if (s_cpu_prev_handle[k] == s_task_stat[i].xHandle) {
                    const uint32_t cur = (uint32_t)s_task_stat[i].ulRunTimeCounter;
                    if (cur >= s_cpu_prev_run[k]) busy_run += cur - s_cpu_prev_run[k];
                    break;
                }
            }
        }
    }

    for (UBaseType_t i = 0; i < n && i < SYSINFO_TASK_MAX; i++) {
        s_cpu_prev_handle[i] = s_task_stat[i].xHandle;
        s_cpu_prev_run[i] = (uint32_t)s_task_stat[i].ulRunTimeCounter;
    }
    s_cpu_prev_count = (n < SYSINFO_TASK_MAX) ? n : SYSINFO_TASK_MAX;
    s_cpu_prev_total = total;
    s_cpu_prev_valid = true;

    const uint32_t x10 = (d_total > 0) ? (uint32_t)((uint64_t)busy_run * 1000u / d_total) : 0;
    *percent = (uint8_t)((x10 > 1000u) ? 100u : (x10 / 10u));

    xSemaphoreGive(s_mux);
    return ESP_OK;
}
