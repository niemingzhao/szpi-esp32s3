/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - OTA 实现
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "svc.ota";

#define OTA_TASK_STACK   8192
#define OTA_TASK_PRIO    5
#define OTA_HTTP_TIMEOUT 15000

static volatile bool s_running = false;
static volatile bool s_abort = false;
static volatile int s_progress = -1;

bool svc_ota_is_running(void)
{
    return s_running;
}

int svc_ota_progress(void)
{
    return s_progress;
}

static void ota_task(void *arg)
{
    char *url = (char *)arg;

    const esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = OTA_HTTP_TIMEOUT,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 只走 HTTPS */
    };
    const esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    esp_app_desc_t new_app;
    if (esp_https_ota_get_img_desc(handle, &new_app) == ESP_OK) {
        ESP_LOGI(TAG, "new firmware: %s %s", new_app.project_name, new_app.version);
    }

    int total = esp_https_ota_get_image_size(handle);
    s_progress = 0;

    while (true) {
        if (s_abort) {
            ESP_LOGW(TAG, "ota aborted by request");
            esp_https_ota_abort(handle);
            goto cleanup;
        }

        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        if (total > 0) {
            int rd = esp_https_ota_get_image_len_read(handle);
            if (rd >= 0) {
                s_progress = (int)((int64_t)rd * 100 / total);
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "ota download failed (%s)", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        goto cleanup;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota finish failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    s_progress = 100;
    ESP_LOGI(TAG, "ota success, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

cleanup:
    s_progress = -1;
    s_running = false;
    free(url);
    vTaskDelete(NULL);
}

esp_err_t svc_ota_start(const char *url)
{
    if (url == NULL || url[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (s_running) return ESP_ERR_INVALID_STATE;

    char *copy = strdup(url);
    if (copy == NULL) return ESP_ERR_NO_MEM;

    s_abort = false;
    s_progress = 0;
    s_running = true;

    BaseType_t ok = xTaskCreate(ota_task, "svc_ota", OTA_TASK_STACK, copy, OTA_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        s_running = false;
        s_progress = -1;
        free(copy);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ota started: %s", url);
    return ESP_OK;
}

esp_err_t svc_ota_abort(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;

    s_abort = true;
    return ESP_OK;
}
