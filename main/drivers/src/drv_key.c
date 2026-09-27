/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - BOOT 按键（GPIO0，轮询 + 软件去抖）
 *
 * GPIO0 轮询 + 软件去抖，识别单击 / 双击 / 长按。
 *
 * 每 DRV_KEY_SCAN_MS 读一次电平，连续 DRV_KEY_DEBOUNCE_MS 保持同一电平才认可一次
 * 按下 / 松开，这样机械抖动和很短的毛刺都会被忽略（用边沿中断时，抖动会被
 * 当成第二次按下，导致单击丢失、双击要按得很重才认）。
 *
 * 长按在按住期间达到阈值就发出，不等松手；长按触发后本次按住不再产生
 * 单击。单击要等双击窗口结束才发出，以便区分双击。
 */

#include "drv_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "drv.key";

#define DRV_KEY_BOOT_GPIO         GPIO_NUM_0

#define DRV_KEY_SCAN_MS           10
#define DRV_KEY_DEBOUNCE_MS       30
#define DRV_KEY_DOUBLE_CLICK_MS   400
#define DRV_KEY_LONG_PRESS_MS     1500

static drv_key_cb_t s_cb = NULL;
static void *s_user_data = NULL;
static TaskHandle_t s_task_handle = NULL;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[2048];

static void emit_event(drv_key_evt_t evt)
{
    if (s_cb) {
        s_cb(evt, s_user_data);
    }
}

static void key_task(void *arg)
{
    (void)arg;

    const int debounce_ticks = DRV_KEY_DEBOUNCE_MS / DRV_KEY_SCAN_MS;

    int raw_last = gpio_get_level(DRV_KEY_BOOT_GPIO);
    int raw_stable = debounce_ticks;   /* 上电时按当前电平起步，不产生假事件 */
    int level = raw_last;              /* 已认可的电平，1 = 松开 */
    /* 启动时如果按键已经按住，长按计时从此刻算起：press_tick 若用 0，
     * (now - press_tick) 会把系统 uptime 当成"已按时长"，而任务跑起来时早就过了 1.5 s，
     * 会立刻误报一次长按 */
    TickType_t press_tick = xTaskGetTickCount();
    bool long_fired = false;           /* 本次按住已经报过长按 */
    bool pending_click = false;        /* 已松开一次，等双击窗口结束 */
    TickType_t click_deadline = 0;

    while (true) {
        const int raw = gpio_get_level(DRV_KEY_BOOT_GPIO);
        const TickType_t now = xTaskGetTickCount();

        if (raw != raw_last) {
            raw_last = raw;
            raw_stable = 0;
        } else if (raw_stable < debounce_ticks) {
            raw_stable++;
        }

        if (raw_stable >= debounce_ticks && raw != level) {
            level = raw;
            if (level == 0) {
                press_tick = now;
                long_fired = false;
            } else if (!long_fired) {
                if (pending_click) {
                    pending_click = false;
                    emit_event(DRV_KEY_EVT_DOUBLE_CLICK);
                } else {
                    /* 先挂起，等双击窗口结束再确认为单击 */
                    pending_click = true;
                    click_deadline = now + pdMS_TO_TICKS(DRV_KEY_DOUBLE_CLICK_MS);
                }
            }
        }

        /* 长按：按住期间到点就发（上层据此立刻弹电源菜单） */
        if (level == 0 && !long_fired &&
            (uint32_t)(now - press_tick) * portTICK_PERIOD_MS >= DRV_KEY_LONG_PRESS_MS) {
            long_fired = true;
            pending_click = false;     /* 长按优先，吞掉双击等待 */
            emit_event(DRV_KEY_EVT_LONG_PRESS);
        }

        if (pending_click && (int32_t)(now - click_deadline) >= 0) {
            pending_click = false;
            emit_event(DRV_KEY_EVT_CLICK);
        }

        vTaskDelay(pdMS_TO_TICKS(DRV_KEY_SCAN_MS));
    }
}

esp_err_t drv_key_init(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;  /* 已初始化 */
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(DRV_KEY_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,   /* 轮询，不用中断 */
    };
    esp_err_t err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_task_handle = xTaskCreateStaticPinnedToCore(
        key_task,
        "key_task",
        2048,
        NULL,
        4,
        s_task_stack,
        &s_task_buffer,
        0);   /* 核心 0 */

    if (s_task_handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "BOOT key initialized (GPIO0, poll %d ms)", DRV_KEY_SCAN_MS);
    return ESP_OK;
}

esp_err_t drv_key_register_callback(drv_key_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_user_data = user_data;
    return ESP_OK;
}
