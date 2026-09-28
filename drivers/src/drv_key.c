/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - BOOT Key Driver
 *
 * GPIO0 下降沿中断，检测单击/双击/长按/极长按
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
#define DOUBLE_CLICK_MS        400
#define LONG_PRESS_MS          1500
#define VERY_LONG_PRESS_MS     3000

static drv_key_cb_t s_cb = NULL;
static void *s_user_data = NULL;
static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_evt_queue = NULL;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[2048];

typedef struct {
    uint64_t press_time;
} key_evt_t;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    BaseType_t high_task_wakeup = pdFALSE;
    uint64_t now = esp_timer_get_time() / 1000;  // ms

    key_evt_t evt = { .press_time = now };
    xQueueSendFromISR(s_evt_queue, &evt, &high_task_wakeup);

    if (high_task_wakeup) {
        portYIELD_FROM_ISR();
    }
}

static void key_task(void *arg)
{
    key_evt_t last_press = { 0 };
    key_evt_t prev_release = { 0 };

    uint64_t last_release_time = 0;
    bool first_press = true;

    while (true) {
        key_evt_t evt;
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            uint64_t now = esp_timer_get_time() / 1000;

            // 读取当前 GPIO 电平判断是按下还是释放
            // 下降沿触发 = 按下
            int level = gpio_get_level(BOOT_KEY_GPIO);

            if (level == 0) {
                // 按下
                last_press.press_time = now;
                first_press = false;
            } else {
                // 释放
                uint64_t press_duration = now - last_press.press_time;

                if (press_duration < DEBOUNCE_MS) {
                    // 噪声，忽略
                    continue;
                }

                uint64_t gap = last_press.press_time - prev_release.press_time;
                prev_release.press_time = last_press.press_time;

                if (gap < DOUBLE_CLICK_MS && !first_press) {
                    // 双击
                    if (s_cb) {
                        s_cb(DRV_KEY_EVT_DOUBLE_CLICK, s_user_data);
                    }
                    first_press = true;
                } else if (press_duration >= VERY_LONG_PRESS_MS) {
                    // 极长按
                    if (s_cb) {
                        s_cb(DRV_KEY_EVT_VERY_LONG_PRESS, s_user_data);
                    }
                    first_press = true;
                } else if (press_duration >= LONG_PRESS_MS) {
                    // 长按
                    if (s_cb) {
                        s_cb(DRV_KEY_EVT_LONG_PRESS, s_user_data);
                    }
                    first_press = true;
                }

                last_release_time = now;
            }
        }
    }
}

esp_err_t drv_key_init(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;  // 已初始化
    }

    // 创建队列
    s_evt_queue = xQueueCreate(8, sizeof(key_evt_t));

    // 配置 GPIO0 下降沿中断，上拉
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(BOOT_KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // 双向触发以便检测按下和释放
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));

    // 安装全局 GPIO ISR 服务
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    // 安装 ISR handler
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOOT_KEY_GPIO, gpio_isr_handler, NULL));

    // 创建按键任务
    s_task_handle = xTaskCreateStaticPinnedToCore(
        key_task,
        "key_task",
        2048,
        NULL,
        4,  // 优先级
        s_task_stack,
        &s_task_buffer,
        tskNO_AFFINITY  // xCoreID: 不绑定特定核心
    );

    ESP_LOGI(TAG, "BOOT key initialized (GPIO0)");
    return ESP_OK;
}

esp_err_t drv_key_register_callback(drv_key_cb_t cb, void *user_data)
{
    s_cb = cb;
    s_user_data = user_data;
    return ESP_OK;
}
