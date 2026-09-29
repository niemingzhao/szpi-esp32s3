/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Settings 实现（NVS 键值存储）
 */

#include "svc_common.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "svc.settings";

esp_err_t svc_settings_init(void)
{
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_settings_set_i32(const char *ns, const char *key, int32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t svc_settings_get_i32(const char *ns, const char *key, int32_t *val, int32_t def)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (val) *val = def;
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    int32_t v = def;
    err = nvs_get_i32(h, key, &v);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (val) *val = def;
        return ESP_OK;
    }
    if (err == ESP_OK && val) *val = v;
    return err;
}

esp_err_t svc_settings_set_u32(const char *ns, const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t svc_settings_get_u32(const char *ns, const char *key, uint32_t *val, uint32_t def)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (val) *val = def;
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    uint32_t v = def;
    err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (val) *val = def;
        return ESP_OK;
    }
    if (err == ESP_OK && val) *val = v;
    return err;
}

esp_err_t svc_settings_set_u8(const char *ns, const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t svc_settings_get_u8(const char *ns, const char *key, uint8_t *val, uint8_t def)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (val) *val = def;
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    uint8_t v = def;
    err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (val) *val = def;
        return ESP_OK;
    }
    if (err == ESP_OK && val) *val = v;
    return err;
}

esp_err_t svc_settings_set_str(const char *ns, const char *key, const char *val)
{
    if (val == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t svc_settings_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def)
{
    if (buf == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    buf[0] = '\0';   /* 任何失败路径都保证是空串，避免上层读到未初始化内容 */

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (def) strlcpy(buf, def, len);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    size_t l = len;
    err = nvs_get_str(h, key, buf, &l);
    nvs_close(h);
    if (err != ESP_OK) {
        if (def) strlcpy(buf, def, len);
        else     buf[0] = '\0';
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    return ESP_OK;
}
