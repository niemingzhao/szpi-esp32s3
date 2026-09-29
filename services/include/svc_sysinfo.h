/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 系统信息 / 异常记录 / 性能监控
 *
 * 把"运行状态"集中到一处，供 Debug App / 关于页使用；App 不必直接调 IDF。
 *   - SYS-006：系统信息查询
 *   - SYS-005：崩溃现场记录（RAM 暂存 -> 下次启动写入 NVS，串口可读）
 *   - DBG-003：任务 CPU / 栈占用快照
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 崩溃记录最多保留最近几条 */
#define SVC_SYSINFO_CRASH_KEEP 3

typedef struct {
    const char *idf_version;      /* 如 "v5.4.4" */
    const char *reset_reason;     /* "power-on" / "software" / "panic" 等 */
    uint32_t uptime_s;            /* 开机至今秒数 */
    size_t heap_internal_free;    /* 内部 SRAM 剩余字节 */
    size_t heap_internal_min;     /* 内部 SRAM 历史最低（判断内存泄漏） */
    size_t heap_psram_free;       /* PSRAM 剩余字节 */
    uint8_t chip_cores;
    uint8_t chip_revision;
    /* 以下供"关于本机"使用 */
    const char *project_name;     /* 工程名 */
    const char *app_version;      /* 应用版本（git describe 或 CONFIG_APP_PROJECT_VER） */
    const char *build_date;       /* 编译日期 */
    const char *build_time;       /* 编译时间 */
    const char *chip_model;       /* "ESP32-S3" 等 */
    char mac[18];                 /* Wi-Fi STA MAC，形如 "9c:13:9e:8a:ca:20" */
    uint32_t flash_size;          /* 外部 flash 容量（字节） */
} svc_sysinfo_t;

/** 单个任务的运行状态（DBG-003） */
typedef struct {
    char name[16];                /* 任务名 */
    uint8_t cpu_percent;          /* 两次采样之间占用的 CPU 百分比 */
    uint32_t stack_free;          /* 栈剩余高水位（字节） */
    int8_t core;                  /* 绑定核心 0/1，-1 = 不限制 */
} svc_sysinfo_task_t;

/**
 * @brief 初始化（读走上次崩溃记录并写入 NVS）
 */
esp_err_t svc_sysinfo_init(void);

/**
 * @brief 采集一次系统信息
 */
esp_err_t svc_sysinfo_get(svc_sysinfo_t *out);

/**
 * @brief 复位原因的可读字符串
 */
const char *svc_sysinfo_reset_reason_str(void);

/**
 * @brief 读取已保存的崩溃记录（最近若干条，'\n' 分隔）
 *
 * 没有记录时 buf 置空并返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t svc_sysinfo_get_crash_log(char *buf, size_t len);

/**
 * @brief 清除已保存的崩溃记录
 */
esp_err_t svc_sysinfo_clear_crash_log(void);

/**
 * @brief 任务运行状态快照（按 CPU 占用降序）
 *
 * 首次调用没有对比基准，cpu_percent 全为 0；之后按两次调用之间的差值计算。
 */
esp_err_t svc_sysinfo_get_tasks(svc_sysinfo_task_t *out, size_t max, size_t *count);

/**
 * @brief 总体 CPU 占用百分比（不含 idle 任务；首次调用返回 0）
 */
esp_err_t svc_sysinfo_get_cpu_usage(uint8_t *percent);

/**
 * @brief 最近日志的尾部（供调试控制台显示，'\0' 结尾）
 *
 * 从开机起把 ESP_LOGx 输出同时抄进一个无锁环形缓冲（无锁 = 任何上下文都能安全写入），
 * 这里返回其中最后的内容；缓冲区大小见实现里的 SYSINFO_LOG_RING_SIZE（2 KB）。
 * 没有任何日志时返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t svc_sysinfo_get_recent_logs(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
