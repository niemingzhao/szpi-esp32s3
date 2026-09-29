/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Touch (FT6336)
 *
 * 设计：本模块是触摸设备的唯一轮询者。
 *   - touch_scan_task 周期读取 esp_lcd_touch 并缓存最新坐标，同时做手势识别
 *   - 注册一个 LVGL POINTER input device，其 read_cb 只读取缓存，不访问硬件
 *   - 对外 periph_touch_read() 返回同一份缓存
 * 这样避免了 LVGL 与手势任务并发读取同一设备导致丢点。
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
#include <stdlib.h>

static const char *TAG = "periph.touch";

#define TOUCH_SCAN_PERIOD_MS   10
#define TAP_MAX_TIME_MS        200
#define DOUBLE_TAP_MS          300
#define LONG_PRESS_TIME_MS     500
#define SWIPE_THRESHOLD_PX     50

static esp_lcd_touch_handle_t s_touch = NULL;
static lv_indev_t *s_indev = NULL;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static periph_touch_point_t s_latest = { 0 };

static periph_touch_cb_t s_cb = NULL;
static void *s_user_data = NULL;

static void touch_indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
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
    bool swiped = false;
    uint64_t press_start = 0;
    periph_touch_point_t swipe_start = { 0 };

    bool pending_tap = false;
    uint64_t pending_tap_time = 0;

    uint16_t last_x = 0, last_y = 0;

    while (true) {
        esp_lcd_touch_point_data_t pts[1] = { 0 };
        uint8_t cnt = 0;

        if (s_touch != NULL) {
            esp_lcd_touch_read_data(s_touch);
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
            .touch_id = 0,
        };

        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_latest = pt;
            xSemaphoreGive(s_mutex);
        }

        uint64_t now_ms = esp_timer_get_time() / 1000;

        if (pt.pressed) {
            if (!was_pressed) {
                press_start = now_ms;
                swipe_start = pt;
                swiped = false;
                long_sent = false;
                was_pressed = true;
                if (s_cb) s_cb(PERIPH_TOUCH_EVT_PRESS, &pt, s_user_data);
            } else {
                int dx = (int)pt.x - (int)swipe_start.x;
                int dy = (int)pt.y - (int)swipe_start.y;

                if (!swiped && s_cb) {
                    periph_touch_evt_t evt;
                    bool detected = true;
                    if (dx > SWIPE_THRESHOLD_PX && abs(dy) < SWIPE_THRESHOLD_PX) {
                        evt = PERIPH_TOUCH_EVT_SWIPE_RIGHT;
                    } else if (dx < -SWIPE_THRESHOLD_PX && abs(dy) < SWIPE_THRESHOLD_PX) {
                        evt = PERIPH_TOUCH_EVT_SWIPE_LEFT;
                    } else if (dy < -SWIPE_THRESHOLD_PX && abs(dx) < SWIPE_THRESHOLD_PX) {
                        evt = PERIPH_TOUCH_EVT_SWIPE_UP;
                    } else if (dy > SWIPE_THRESHOLD_PX && abs(dx) < SWIPE_THRESHOLD_PX) {
                        evt = PERIPH_TOUCH_EVT_SWIPE_DOWN;
                    } else {
                        detected = false;
                    }
                    if (detected) {
                        s_cb(evt, &pt, s_user_data);
                        swiped = true;
                    }
                }

                if (!long_sent && !swiped &&
                    (now_ms - press_start) >= LONG_PRESS_TIME_MS) {
                    long_sent = true;
                    if (s_cb) s_cb(PERIPH_TOUCH_EVT_LONG_PRESS, &pt, s_user_data);
                }
            }
        } else {
            if (was_pressed) {
                uint64_t dur = now_ms - press_start;
                was_pressed = false;
                if (s_cb) s_cb(PERIPH_TOUCH_EVT_RELEASE, &pt, s_user_data);

                if (!swiped && dur < TAP_MAX_TIME_MS) {
                    if (pending_tap && (now_ms - pending_tap_time) < DOUBLE_TAP_MS) {
                        pending_tap = false;
                        if (s_cb) s_cb(PERIPH_TOUCH_EVT_DOUBLE_TAP, &pt, s_user_data);
                    } else {
                        pending_tap = true;
                        pending_tap_time = now_ms;
                    }
                }
            }

            /* 双击窗口结束，确认为单击 */
            if (pending_tap && (now_ms - pending_tap_time) >= DOUBLE_TAP_MS) {
                pending_tap = false;
                if (s_cb) s_cb(PERIPH_TOUCH_EVT_TAP, &s_latest, s_user_data);
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
    lv_disp_t *disp = periph_lcd_get_disp();
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_STATE, TAG, "LCD display not ready");

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");

    static lv_indev_drv_t indev_drv;
    if (lvgl_port_lock(0)) {
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_indev_read_cb;
        indev_drv.disp = disp;
        s_indev = lv_indev_drv_register(&indev_drv);
        lvgl_port_unlock();
    }
    ESP_RETURN_ON_FALSE(s_indev != NULL, ESP_FAIL, TAG, "lv_indev_drv_register failed");

    BaseType_t ret = xTaskCreatePinnedToCore(
        touch_scan_task,
        "periph_touch",
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

esp_err_t periph_touch_read(periph_touch_point_t *point)
{
    if (point == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        *point = s_latest;
        xSemaphoreGive(s_mutex);
    }
    return ESP_OK;
}
