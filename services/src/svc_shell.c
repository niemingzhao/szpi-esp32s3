/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 串口命令行实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "svc.shell";

static bool s_started = false;

static int cmd_version(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const esp_app_desc_t *d = esp_app_get_description();
    if (d != NULL) {
        ESP_LOGI(TAG, "firmware: %s %s (%s %s)", d->project_name, d->version, d->date, d->time);
    }
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
    return 0;
}

static int cmd_sysinfo(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) != ESP_OK) return 1;

    ESP_LOGI(TAG, "idf=%s reset=%s uptime=%us", si.idf_version, si.reset_reason, (unsigned)si.uptime_s);
    ESP_LOGI(TAG, "heap internal free=%u min=%u, psram free=%u",
             (unsigned)si.heap_internal_free, (unsigned)si.heap_internal_min,
             (unsigned)si.heap_psram_free);
    ESP_LOGI(TAG, "chip: %u core(s), rev %u", (unsigned)si.chip_cores, (unsigned)si.chip_revision);
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    svc_net_status_t st;
    if (svc_net_get_status(&st) != ESP_OK) return 1;

    ESP_LOGI(TAG, "wifi: %s ssid='%s' ip='%s' rssi=%d",
             st.wifi_connected ? "connected" : "disconnected",
             st.wifi_ssid, st.ip_addr, (int)st.rssi);
    return 0;
}

static int cmd_bt(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    svc_bt_diag_t d;
    if (svc_bt_get_diag(&d) != ESP_OK) return 1;

    ESP_LOGI(TAG, "ble: state=%s adv(want=%d ready=%d active=%d) scan_ready=%d connected=%d",
             svc_bt_state_name(d.state), (int)d.adv_want, (int)d.adv_ready, (int)d.adv_active,
             (int)d.scan_ready, (int)d.connected);
    ESP_LOGI(TAG, "ble: gap events=%u last=%u, hid ready=%d (reports need connection + pairing)",
             (unsigned)d.gap_events, (unsigned)d.gap_last, (int)svc_bt_hid_is_ready());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ESP_LOGW(TAG, "reboot requested from console");
    svc_power_request_reboot();
    return 0;
}

static int cmd_crash(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* 仅用于验收 SYS-005：触发一次崩溃，重启后应能从串口日志 / NVS 读到现场 */
    ESP_LOGE(TAG, "crash test requested from console");
    abort();
    return 0;
}

esp_err_t svc_shell_start(void)
{
    if (s_started) return ESP_OK;

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "szpi-os>";
    repl_cfg.max_cmdline_length = 128;

    const esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t cmds[] = {
        { .command = "version", .help = "show firmware / IDF version", .func = cmd_version },
        { .command = "sysinfo", .help = "show uptime / heap / reset reason", .func = cmd_sysinfo },
        { .command = "wifi",    .help = "show wifi status", .func = cmd_wifi },
        { .command = "bt",      .help = "show BLE state / HID readiness", .func = cmd_bt },
        { .command = "reboot",  .help = "reboot the device", .func = cmd_reboot },
        { .command = "crash",   .help = "trigger a crash (SYS-005 self test)", .func = cmd_crash },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "console started (version / sysinfo / wifi / bt / reboot / crash)");
    return ESP_OK;
}
