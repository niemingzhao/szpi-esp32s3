/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS v0.3 - Drivers + Peripherals Layer Integration
 *
 * 启动序列：
 *   NVS → bsp_init()（Drivers）→ peripherals_init_all()（含 LVGL display/touch）
 *   → 创建 UI → 挂载内置 Flash（首次自动格式化）
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "drv_common.h"
#include "periph_common.h"
#include "svc_common.h"
#include "lvgl.h"

static const char *TAG = "szpi-os";

/* ============ UI ============ */

static lv_obj_t *s_label = NULL;
static int s_click_count = 0;

static void btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        s_click_count++;
        lvgl_port_lock(0);
        lv_label_set_text_fmt(s_label, "Hello SZPI-OS!\nClicked: %d", s_click_count);
        lvgl_port_unlock();
        ESP_LOGI(TAG, "Button clicked, count=%d", s_click_count);
    }
}

static void create_ui(void)
{
    lvgl_port_lock(0);

    // 深色背景
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x121212), 0);

    // 标题
    lv_obj_t *title = lv_label_create(lv_scr_act());
    lv_label_set_text(title, "SZPI-OS");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // 按钮
    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 200, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

    // 按钮标签
    s_label = lv_label_create(btn);
    lv_label_set_text(s_label, "Tap me!");
    lv_obj_center(s_label);

    // 底部状态栏
    lv_obj_t *status = lv_label_create(lv_scr_act());
    lv_label_set_text(status, "v0.3 - Peripherals Layer");
    lv_obj_set_style_text_color(status, lv_color_hex(0xBBBBBB), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -20);

    lvgl_port_unlock();
}

/* ============ 主函数 ============ */

void app_main(void)
{
    ESP_LOGI(TAG, "===== SZPI-OS Boot =====");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());

    // 1. NVS 初始化
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Drivers 层初始化（I2C / SPI / LEDC / PCA9557 / LCD / Touch / Key / IMU）
    //    handle 由 Peripherals 层通过 drv_*_get_* 获取，此处不需要
    ESP_ERROR_CHECK(bsp_init(NULL, NULL, NULL));

    // 3. Peripherals 层初始化（含 LVGL display 与触摸 input device）
    ESP_ERROR_CHECK(peripherals_init_all());

    // 4. Services 层初始化
    ESP_ERROR_CHECK(services_init());

    // 5. 创建 UI（背光已由 periph_lcd_init 按 NVS 亮度和默认值打开）
    create_ui();
    ESP_LOGI(TAG, "UI created");

    // 6. 挂载内置 Flash 文件系统（首次自动格式化；放在 UI 之后避免阻塞首屏）
    if (periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH) != ESP_OK) {
        ESP_LOGW(TAG, "internal storage mount failed");
    }

    ESP_LOGI(TAG, "===== Ready =====");
}
