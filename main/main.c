/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS v0.2 - Drivers Layer Integration
 * 使用 drivers/ 层初始化硬件，LVGL 启动屏幕 + 按钮
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_lvgl_port.h"
#include "drv_common.h"
#include "lvgl.h"

static const char *TAG = "szpi-os";

/* ============ 静态变量 ============ */

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;

/* ============ LVGL 初始化 ============ */

static esp_err_t bsp_lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = DRV_LCD_H_RES * 20,
        .double_buffer = false,
        .hres = DRV_LCD_H_RES,
        .vres = DRV_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_LOGI(TAG, "LVGL display added");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = s_touch,
    };
    lvgl_port_add_touch(&touch_cfg);
    ESP_LOGI(TAG, "LVGL touch added");

    return ESP_OK;
}

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
    lv_label_set_text(status, "v0.2 - Drivers Layer");
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

    // 2. BSP 初始化（Drivers 层）
    ESP_ERROR_CHECK(bsp_init(&s_lcd_panel, &s_lcd_io, &s_touch));

    // 3. LVGL 初始化
    ESP_ERROR_CHECK(bsp_lvgl_init());

    // 4. 打开背光 (80%)
    drv_ledc_set_brightness(80);
    ESP_LOGI(TAG, "Backlight on (80%%)");

    // 5. 创建 UI
    create_ui();
    ESP_LOGI(TAG, "UI created");
    ESP_LOGI(TAG, "===== Ready =====");
}
