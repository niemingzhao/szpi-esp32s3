/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Settings (NVS-backed key/value configuration)
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化配置系统
 */
esp_err_t svc_settings_init(void);

/* int32 */
esp_err_t svc_settings_set_i32(const char *ns, const char *key, int32_t val);
esp_err_t svc_settings_get_i32(const char *ns, const char *key, int32_t *val, int32_t def);

/* uint32 */
esp_err_t svc_settings_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t svc_settings_get_u32(const char *ns, const char *key, uint32_t *val, uint32_t def);

/* uint8 */
esp_err_t svc_settings_set_u8(const char *ns, const char *key, uint8_t val);
esp_err_t svc_settings_get_u8(const char *ns, const char *key, uint8_t *val, uint8_t def);

/* string */
esp_err_t svc_settings_set_str(const char *ns, const char *key, const char *val);
esp_err_t svc_settings_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def);

/* blob：get 时 *len 进为缓冲大小、出为实际长度 */
esp_err_t svc_settings_set_blob(const char *ns, const char *key, const void *data, size_t len);
esp_err_t svc_settings_get_blob(const char *ns, const char *key, void *buf, size_t *len);

#ifdef __cplusplus
}
#endif
