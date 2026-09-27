/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 触摸（FT6336 单点，轮询 + 缓存）
 *
 * 设计：本模块是触摸设备的唯一轮询者。
 *   - touch_scan_task 周期读取 esp_lcd_touch 并缓存最新坐标，同时做点击 / 长按判定
 *   - 注册一个 LVGL POINTER input device，其 read_cb 只读取缓存，不访问硬件
 * 这样避免了 LVGL 与触摸任务并发读取同一设备导致丢点。
 *
 * FT6336 为单点触摸，只提供按下 / 抬起 / 点击 / 长按。
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "periph.touch";

#define TOUCH_SCAN_PERIOD_MS   10
#define LONG_PRESS_TIME_MS     500

static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_t *s_indev = NULL;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static periph_touch_point_t s_latest = { 0 };

static periph_touch_cb_t s_cb = NULL;
static void *s_user_data = NULL;
static volatile bool s_drop_gesture = false;   /* 丢弃这次触摸（熄屏唤醒，直到抬手） */

static void touch_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        data->point.x = s_latest.x;
        data->point.y = s_latest.y;
        data->state = s_latest.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        xSemaphoreGive(s_mutex);
    }
}

static void touch_scan_task(void *arg)
{
    (void)arg;

    bool was_pressed = false;
    bool long_sent = false;
    uint64_t press_start = 0;
    uint16_t last_x = 0, last_y = 0;

    while (true) {
        esp_lcd_touch_point_data_t pts[1] = { 0 };
        uint8_t cnt = 0;

        if (s_touch != NULL) {
            esp_err_t rerr = esp_lcd_touch_read_data(s_touch);
            if (rerr != ESP_OK) {
                static bool s_read_err_logged = false;
                if (!s_read_err_logged) {
                    s_read_err_logged = true;
                    ESP_LOGW(TAG, "touch read failed: %s", esp_err_to_name(rerr));
                }
            }
            esp_lcd_touch_get_data(s_touch, pts, &cnt, 1);
        }
        if (cnt > 0) {
            last_x = pts[0].x;
            last_y = pts[0].y;
        }

        periph_touch_point_t pt = {
            .x = last_x,
            .y = last_y,
            .pressed = (cnt > 0),
        };

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            if (s_drop_gesture) {
                s_latest.pressed = false;      /* 丢弃：让 LVGL 只看到"没按下" */
            } else {
                s_latest = pt;
            }
            xSemaphoreGive(s_mutex);
        }

        uint64_t now_ms = esp_timer_get_time() / 1000;

        if (pt.pressed) {
            if (!was_pressed) {
                press_start = now_ms;
                long_sent = false;
                was_pressed = true;
                if (s_cb) s_cb(PERIPH_TOUCH_EVT_PRESS, &pt, s_user_data);
            } else if (!long_sent && (now_ms - press_start) >= LONG_PRESS_TIME_MS) {
                long_sent = true;
                if (s_cb) s_cb(PERIPH_TOUCH_EVT_LONG_PRESS, &pt, s_user_data);
            }
        } else if (was_pressed) {
            uint64_t dur = now_ms - press_start;
            was_pressed = false;
            s_drop_gesture = false;                /* 这次触摸结束，恢复正常输入 */
            if (s_cb) s_cb(PERIPH_TOUCH_EVT_RELEASE, &pt, s_user_data);

            /* 长按阈值内抬起才算点击 */
            if (!long_sent && dur < LONG_PRESS_TIME_MS && s_cb) {
                s_cb(PERIPH_TOUCH_EVT_TAP, &pt, s_user_data);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_SCAN_PERIOD_MS));
    }
}

esp_err_t periph_touch_init(void)
{
    if (s_task_handle != NULL) return ESP_OK;

    ESP_RETURN_ON_ERROR(drv_ft6336_get_touch_handle(&s_touch), TAG, "touch not initialized");

    /* 注册 LVGL input device（回调只读缓存）；先确认显示已就绪，再分配互斥量 */
    lv_display_t *disp = periph_lcd_get_disp();
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_STATE, TAG, "LCD display not ready");

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    if (lvgl_port_lock(0)) {
        s_indev = lv_indev_create();
        if (s_indev != NULL) {
            lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(s_indev, touch_indev_read_cb);
            lv_indev_set_display(s_indev, disp);
        }
        lvgl_port_unlock();
    }
    ESP_RETURN_ON_FALSE(s_indev != NULL, ESP_FAIL, TAG, "lv_indev_create failed");

    BaseType_t ret = xTaskCreatePinnedToCore(
        touch_scan_task,
        "touch_scan_task",
        4096,
        NULL,
        4,
        &s_task_handle,
        0);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t periph_touch_register_callback(periph_touch_cb_t cb, void *user)
{
    s_cb = cb;
    s_user_data = user;
    return ESP_OK;
}

esp_err_t periph_touch_drop_next_gesture(void)
{
    s_drop_gesture = true;
    return ESP_OK;
}
