/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - LCD
 *
 * 职责：
 *   - 基于 drv_st7789 输出的 panel / io handle 初始化 LVGL display
 *   - 背光亮度控制 + NVS 持久化（namespace sys / key brightness）
 *
 * 说明：LCD 面板的硬件初始化由 Drivers 层 bsp_init() 完成，
 *       本模块只负责 LVGL 集成与业务级亮度接口。
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

static const char *TAG = "periph.lcd";

#define NVS_NS "sys"
#define NVS_KEY_BRIGHTNESS "brightness"

#define LVGL_BUFFER_LINES 20

static bool s_initialized = false;
static uint8_t s_brightness = 80;
static lv_disp_t *s_disp = NULL;

static esp_err_t load_brightness(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t val = s_brightness;
    size_t len = sizeof(val);
    err = nvs_get_blob(h, NVS_KEY_BRIGHTNESS, &val, &len);
    if (err == ESP_OK) s_brightness = val;
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_brightness(uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, NVS_KEY_BRIGHTNESS, &val, sizeof(val));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t periph_lcd_init(void)
{
    if (s_initialized) return ESP_OK;

    /* 1. 取 Drivers 层已初始化的 panel / io handle */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(drv_st7789_get_panel_handle(&panel), TAG, "LCD panel not initialized");
    ESP_RETURN_ON_ERROR(drv_st7789_get_io_handle(&io), TAG, "LCD io not initialized");

    /* 2. LVGL port + display（帧缓冲放 PSRAM） */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = DRV_LCD_H_RES * LVGL_BUFFER_LINES,
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

    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");

    /* 默认屏幕设为深色，避免 LVGL 默认浅色主题导致白屏 */
    if (lvgl_port_lock(0)) {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x121212), 0);
        lvgl_port_unlock();
    }

    /* 3. 背光：加载 NVS 中保存的亮度并应用 */
    load_brightness();
    drv_ledc_set_brightness(s_brightness);

    s_initialized = true;
    ESP_LOGI(TAG, "initialized, brightness=%d, %ux%u", s_brightness,
             DRV_LCD_H_RES, DRV_LCD_V_RES);
    return ESP_OK;
}

lv_disp_t *periph_lcd_get_disp(void)
{
    return s_disp;
}

uint16_t periph_lcd_get_width(void)
{
    return DRV_LCD_H_RES;
}

uint16_t periph_lcd_get_height(void)
{
    return DRV_LCD_V_RES;
}

esp_err_t periph_lcd_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_brightness = percent;
    drv_ledc_set_brightness(percent);
    save_brightness(percent);
    return ESP_OK;
}

uint8_t periph_lcd_get_brightness(void)
{
    return s_brightness;
}

esp_err_t periph_lcd_set_backlight(bool on)
{
    drv_ledc_set_brightness(on ? s_brightness : 0);
    return ESP_OK;
}

esp_err_t periph_lcd_set_rotation(periph_lcd_rotation_t rot)
{
    /* v0.3 固定横屏（320×240）；运行时旋转需重建 LVGL display，规划到后续版本 */
    (void)rot;
    ESP_LOGW(TAG, "runtime rotation is not supported in v0.3");
    return ESP_ERR_NOT_SUPPORTED;
}

void periph_lcd_fill(uint16_t color)
{
    esp_lcd_panel_handle_t panel = NULL;
    if (drv_st7789_get_panel_handle(&panel) != ESP_OK) return;

    uint16_t *line = heap_caps_malloc(DRV_LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (line == NULL) {
        ESP_LOGW(TAG, "fill: no mem for line buffer");
        return;
    }
    for (int i = 0; i < DRV_LCD_H_RES; i++) {
        line[i] = color;
    }
    for (int y = 0; y < DRV_LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, DRV_LCD_H_RES, y + 1, line);
    }
    heap_caps_free(line);
}

esp_err_t periph_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t *buf)
{
    if (buf == NULL) return ESP_ERR_INVALID_ARG;

    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = drv_st7789_get_panel_handle(&panel);
    if (err != ESP_OK) return err;

    return esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, buf);
}
