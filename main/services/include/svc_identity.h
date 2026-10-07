/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 产品标识（系统名 / 版本 / 配网热点名 / 蓝牙广播名）
 *
 * 全工程只在这里定义一次：启动日志、「关于本机」、Wi-Fi 配网热点、蓝牙广播名都从这里取值，
 * 改名或改版本只动这一个文件。
 *
 * 定义在 Services 层（Services / Framework / Apps 三层共用）。
 */

#pragma once

/** 系统名（界面显示；配网热点名 / 蓝牙广播名都以它开头） */
#define SZPI_OS_NAME      "SZPI-OS"

/**
 * @brief 固件版本（界面显示）
 *
 * 构建产物的 git 版本另见 svc_sysinfo 的 app_version，两者用途不同：
 * 这里是产品版本号，那里是"这一次构建"的版本。
 */
#define SZPI_OS_VERSION   "v1.0.0"

/**
 * @brief 配网热点名（AP 模式，32 字节以内）
 *
 * 系统名-XXXX：XXXX 是 Wi-Fi STA 的 MAC 后 2 字节（大写十六进制，例如 SZPI-OS-CA20），
 * 和「关于本机」显示的 MAC 同一个，一批板子放一起时靠它区分。实现见 svc_identity.c。
 */
const char *svc_identity_ap_ssid(void);

/**
 * @brief 蓝牙广播名（BLE 设备名 / GAP 名，29 字节以内）
 *
 * 系统名-XXXX：XXXX 是蓝牙主机地址后 2 字节（大写十六进制，例如 SZPI-OS-CA22）。
 * 地址要等蓝牙协议栈使能之后才读得到，在那之前返回纯系统名。实现见 svc_identity.c。
 */
const char *svc_identity_bt_name(void);
