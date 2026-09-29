/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 串口命令行（调试用）
 *
 * 在 UART0（GPIO43/44，接 CH340K）上提供 REPL，命令用于现场排查：
 *   version / sysinfo / wifi / reboot
 * 输出走 ESP_LOG，与系统日志同一路串口。
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动串口命令行（重复调用安全）
 */
esp_err_t svc_shell_start(void);

#ifdef __cplusplus
}
#endif
