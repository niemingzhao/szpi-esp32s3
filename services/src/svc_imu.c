/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IMU 运动 / 姿态实现
 *
 * QMI8658 的中断引脚未引出，用软件轮询代替中断：每 50 ms 读一次，
 * 运动 / 朝向发生变化时发布事件。只在"变化"时发布，避免刷事件总线。
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "svc.imu";

#define IMU_POLL_MS     50
#define IMU_TASK_STACK  3072

static bool s_started = false;

static void imu_task(void *arg)
{
    (void)arg;

    /* 纳入 Task WDT：本任务 50 ms 一圈 */
    if (svc_watchdog_subscribe() != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog subscribe failed");
    }

    bool last_moving = (periph_imu_get_motion() != PERIPH_IMU_MOTION_NONE);
    periph_imu_orientation_t last_orientation = periph_imu_get_orientation();

    while (true) {
        periph_imu_data_t data;

        if (periph_imu_read(&data) == ESP_OK) {
            periph_imu_motion_t motion = periph_imu_get_motion();
            periph_imu_orientation_t orientation = periph_imu_get_orientation();

            bool moving = (motion != PERIPH_IMU_MOTION_NONE);
            if (moving != last_moving) {
                last_moving = moving;
                /* 只在开始运动时发布，停止运动不发 */
                if (moving) {
                    svc_event_bus_publish(SVC_EVENT_IMU_MOTION, &motion, sizeof(motion));
                }
            }

            if (orientation != last_orientation) {
                last_orientation = orientation;
                svc_event_bus_publish(SVC_EVENT_IMU_ORIENTATION, &orientation, sizeof(orientation));
                ESP_LOGI(TAG, "orientation -> %d", (int)orientation);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
        svc_watchdog_feed();
    }
}

esp_err_t svc_imu_init(void)
{
    if (s_started) return ESP_OK;

    if (xTaskCreate(imu_task, "svc_imu", IMU_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "initialized (poll %d ms)", IMU_POLL_MS);
    return ESP_OK;
}

bool svc_imu_is_moving(void)
{
    return (periph_imu_get_motion() != PERIPH_IMU_MOTION_NONE);
}

periph_imu_orientation_t svc_imu_get_orientation(void)
{
    return periph_imu_get_orientation();
}
