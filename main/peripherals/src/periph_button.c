/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 按键封装（BOOT 键 / drv_key）
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"

static const char *TAG = "periph.button";

static periph_button_cb_t s_cb = NULL;
static void *s_user_data = NULL;

static void key_evt_wrapper(drv_key_evt_t evt, void *user)
{
    (void)user;
    if (s_cb == NULL) return;

    periph_button_evt_t periph_evt;
    switch (evt) {
        case DRV_KEY_EVT_CLICK:          periph_evt = PERIPH_BTN_EVT_CLICK;          break;
        case DRV_KEY_EVT_DOUBLE_CLICK:   periph_evt = PERIPH_BTN_EVT_DOUBLE_CLICK;   break;
        case DRV_KEY_EVT_LONG_PRESS:     periph_evt = PERIPH_BTN_EVT_LONG_PRESS;     break;
        default: return;
    }
    s_cb(periph_evt, s_user_data);
}

esp_err_t periph_button_init(void)
{
    ESP_ERROR_CHECK(drv_key_init());
    esp_err_t err = drv_key_register_callback(key_evt_wrapper, NULL);
    if (err == ESP_OK) ESP_LOGI(TAG, "initialized");
    return err;
}

esp_err_t periph_button_register_callback(periph_button_cb_t cb, void *user)
{
    s_cb = cb;
    s_user_data = user;
    return ESP_OK;
}
