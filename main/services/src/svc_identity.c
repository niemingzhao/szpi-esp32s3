/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 产品标识（配网热点名 / 蓝牙广播名）
 *
 * 两个名字都拼成 系统名-XXXX：XXXX 取地址最后 2 字节的大写十六进制 —— 同一批板子放在
 * 一起时靠它区分。Wi-Fi 用 STA 的 MAC（和「关于本机」显示的 MAC 同一个），蓝牙用协议栈的
 * 主机地址（和串口日志里 Bluetooth MAC 同一个）。
 *
 * 字符串拼好就缓存在静态区：内容固定，调用点分布在 BLE 回调、界面、配网流程里，不该每次
 * 都去读 efuse / 问协议栈。
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_mac.h"            /* esp_read_mac / ESP_MAC_WIFI_STA */
#include "esp_bt_device.h"      /* esp_bt_dev_get_address */
#include <stdio.h>

static const char *TAG = "svc.identity";

/* "SZPI-OS-CA20"：系统名 + '-' + 4 位 + 结尾。Wi-Fi SSID 上限 32 字节、蓝牙名 29 字节 */
#define IDENTITY_NAME_MAX   32

/* 地址最后 2 字节的十六进制（大写）。%hhX 让编译器知道最多 2 位，免得被判截断 */
static void suffix_of(const uint8_t *addr, char out[5])
{
    snprintf(out, 5, "%02hhX%02hhX", (unsigned)addr[4], (unsigned)addr[5]);
}

const char *svc_identity_ap_ssid(void)
{
    static char ssid[IDENTITY_NAME_MAX];
    uint8_t mac[6] = { 0 };

    if (ssid[0] != '\0') return ssid;                   /* 只拼一次 */

    /* STA 的 MAC 开机早期就能从 efuse 读出来，不必等 Wi-Fi 启动 */
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char sfx[5];
        suffix_of(mac, sfx);
        snprintf(ssid, sizeof(ssid), SZPI_OS_NAME "-%s", sfx);
    } else {
        ESP_LOGW(TAG, "read wifi mac failed, ap ssid falls back to plain name");
        snprintf(ssid, sizeof(ssid), "%s", SZPI_OS_NAME);
    }
    return ssid;
}

const char *svc_identity_bt_name(void)
{
    static char name[IDENTITY_NAME_MAX];
    const uint8_t *addr = esp_bt_dev_get_address();     /* 协议栈没使能时返回 NULL */

    if (addr != NULL && name[0] == '\0') {
        char sfx[5];
        suffix_of(addr, sfx);
        snprintf(name, sizeof(name), SZPI_OS_NAME "-%s", sfx);
    }
    return (name[0] != '\0') ? name : SZPI_OS_NAME;     /* 还没起来先给纯系统名 */
}
