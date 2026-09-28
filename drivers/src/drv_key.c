/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - BOOT Key Driver
 *
 * GPIO0 任意边沿中断 + 软件状态机，识别单击 / 双击 / 长按 / 极长按。
 * ISR 只投递事件，时间判定与回调都在 key_task 中完成。
 */

#include "drv_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

static const char *TAG = "drv.key";

#define BOOT_KEY_GPIO         GPIO_NUM_0

#define DEBOUNCE_MS           50
#define DOUBLE_CLICK_MS       400
#define LONG_PRESS_MS         1500
#define VERY_LONG_PRESS_MS    3000

static drv_key_cb_t s_cb = NULL;
static void *s_user_data = NULL;
static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_evt_queue = NULL;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[2048];

/* ISR 只发送一个令牌，具体电平由任务读取 */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    (void)arg;
    BaseType_t high_task_wakeup = pdFALSE;
    uint32_t token = 1;
    xQueueSendFromISR(s_evt_queue, &token, &high_task_wakeup);

    if (high_task_wakeup) {
        portYIELD_FROM_ISR();
    }
}

static void emit_event(drv_key_evt_t evt)
{
    if (s_cb) {
        s_cb(evt, s_user_data);
    }
}

static void key_task(void *arg)
{
    (void)arg;

    bool pending_click = false;   /* 首次短按后，等待双击窗口结束 */
    bool waiting_double = false;  /* 双击窗口内发生了第二次按下 */
    TickType_t click_deadline = 0;
    TickType_t press_tick = 0;

    while (true) {
        TickType_t timeout = portMAX_DELAY;
        if (pending_click && !waiting_double) {
            TickType_t now = xTaskGetTickCount();
            timeout = (click_deadline > now) ? (click_deadline - now) : 0;
        }

        uint32_t token;
        if (xQueueReceive(s_evt_queue, &token, timeout) == pdTRUE) {
            int level = gpio_get_level(BOOT_KEY_GPIO);
            TickType_t now = xTaskGetTickCount();

            if (level == 0) {
                /* 按下：若正处于双击窗口，则本次为第二击 */
                if (pending_click) {
                    pending_click = false;
                    waiting_double = true;
                }
                press_tick = now;
            } else {
                /* 释放 */
                uint32_t dur_ms = (uint32_t)(now - press_tick) * portTICK_PERIOD_MS;

                if (dur_ms < DEBOUNCE_MS) {
                    continue;  /* 抖动，忽略 */
                }

                if (dur_ms >= VERY_LONG_PRESS_MS) {
                    emit_event(DRV_KEY_EVT_VERY_LONG_PRESS);
                    pending_click = false;
                    waiting_double = false;
                } else if (dur_ms >= LONG_PRESS_MS) {
                    emit_event(DRV_KEY_EVT_LONG_PRESS);
                    pending_click = false;
                    waiting_double = false;
                } else if (waiting_double) {
                    emit_event(DRV_KEY_EVT_DOUBLE_CLICK);
                    waiting_double = false;
                } else {
                    /* 短按：延迟到双击窗口结束再判定为单击 */
                    pending_click = true;
                    click_deadline = now + pdMS_TO_TICKS(DOUBLE_CLICK_MS);
                }
            }
        } else if (pending_click) {
            /* 双击窗口超时，确认为单击 */
            emit_event(DRV_KEY_EVT_CLICK);
            pending_click = false;
        }
    }
}

esp_err_t drv_key_init(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;  /* 已初始化 */
    }

    s_evt_queue = xQueueCreate(8, sizeof(uint32_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(BOOT_KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  /* 双向触发，任务据此判定按下/释放 */
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));

    /* 安装全局 GPIO ISR 服务（可能已被其他驱动安装） */
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(gpio_isr_handler_add(BOOT_KEY_GPIO, gpio_isr_handler, NULL));

    s_task_handle = xTaskCreateStaticPinnedToCore(
        key_task,
        "key_task",
        2048,
        NULL,
        4,
        s_task_stack,
        &s_task_buffer,
        tskNO_AFFINITY);

    if (s_task_handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT key initialized (GPIO0)");
    return ESP_OK;
}

esp_err_t drv_key_register_callback(drv_key_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_user_data = user_data;
    return ESP_OK;
}
