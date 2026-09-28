/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - Power 实现（背光超时 / 休眠唤醒 / 重启关机）
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "svc.power";

#define SVC_POWER_DEFAULT_TIMEOUT_S   30

static uint32_t s_timeout_s = SVC_POWER_DEFAULT_TIMEOUT_S;
static bool s_sleeping = false;
static volatile uint64_t s_last_activity_ms = 0;

static void touch_activity_cb(periph_touch_evt_t evt, const periph_touch_point_t *pt, void *user)
{
    (void)user;
    s_last_activity_ms = (uint64_t)(esp_timer_get_time() / 1000);

    if (evt == PERIPH_TOUCH_EVT_PRESS || evt == PERIPH_TOUCH_EVT_TAP) {
        svc_event_bus_publish(SVC_EVENT_TOUCH, (void *)pt,
                              pt ? sizeof(periph_touch_point_t) : 0);
    } else if (evt == PERIPH_TOUCH_EVT_SWIPE_LEFT) {
        svc_event_bus_publish(SVC_EVENT_GESTURE_SWIPE_LEFT, NULL, 0);
    } else if (evt == PERIPH_TOUCH_EVT_SWIPE_RIGHT) {
        svc_event_bus_publish(SVC_EVENT_GESTURE_SWIPE_RIGHT, NULL, 0);
    } else if (evt == PERIPH_TOUCH_EVT_SWIPE_UP) {
        svc_event_bus_publish(SVC_EVENT_GESTURE_SWIPE_UP, NULL, 0);
    } else if (evt == PERIPH_TOUCH_EVT_SWIPE_DOWN) {
        svc_event_bus_publish(SVC_EVENT_GESTURE_SWIPE_DOWN, NULL, 0);
    }

    if (s_sleeping) {
        svc_power_wake();
    }
}

static void power_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_timeout_s == 0 || s_sleeping) continue;

        uint64_t now = (uint64_t)(esp_timer_get_time() / 1000);
        if ((now - s_last_activity_ms) >= (uint64_t)s_timeout_s * 1000) {
            svc_power_sleep();
        }
    }
}

esp_err_t svc_power_init(void)
{
    uint32_t t = SVC_POWER_DEFAULT_TIMEOUT_S;
    svc_settings_get_u32("sys", "bl_timeout", &t, SVC_POWER_DEFAULT_TIMEOUT_S);
    s_timeout_s = t;
    s_last_activity_ms = (uint64_t)(esp_timer_get_time() / 1000);

    periph_touch_register_callback(touch_activity_cb, NULL);
    xTaskCreate(power_task, "power_task", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "initialized (backlight timeout=%us)", (unsigned)s_timeout_s);
    return ESP_OK;
}

esp_err_t svc_power_set_backlight_timeout(uint32_t seconds)
{
    s_timeout_s = seconds;
    svc_settings_set_u32("sys", "bl_timeout", seconds);
    return ESP_OK;
}

uint32_t svc_power_get_backlight_timeout(void)
{
    return s_timeout_s;
}

esp_err_t svc_power_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;

    esp_err_t err = periph_lcd_set_brightness(percent);
    if (err == ESP_OK) {
        svc_event_bus_publish(SVC_EVENT_BRIGHTNESS_CHANGED, &percent, sizeof(percent));
    }
    return err;
}

uint8_t svc_power_get_brightness(void)
{
    return periph_lcd_get_brightness();
}

esp_err_t svc_power_wake(void)
{
    if (s_sleeping) {
        periph_lcd_set_backlight(true);
        s_sleeping = false;
        ESP_LOGI(TAG, "wake");
    }
    s_last_activity_ms = (uint64_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

esp_err_t svc_power_sleep(void)
{
    if (!s_sleeping) {
        periph_lcd_set_backlight(false);
        s_sleeping = true;
        ESP_LOGI(TAG, "sleep");
    }
    return ESP_OK;
}

bool svc_power_is_sleeping(void)
{
    return s_sleeping;
}

esp_err_t svc_power_request_reboot(void)
{
    ESP_LOGI(TAG, "reboot requested");
    esp_restart();
    return ESP_OK;
}

esp_err_t svc_power_request_shutdown(void)
{
    ESP_LOGI(TAG, "shutdown requested");
    svc_event_bus_publish(SVC_EVENT_SHUTDOWN_REQUEST, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();   /* 本板无电池，用 deep sleep 模拟关机 */
    return ESP_OK;
}
