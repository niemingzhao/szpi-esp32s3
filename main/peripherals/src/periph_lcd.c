/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - LCD
 *
 * 职责：
 *   - 基于 drv_st7789 输出的 panel / io handle 初始化 LVGL display
 *   - 背光亮度控制（亮度的持久化由 Services 层 svc_power 负责）
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

static const char *TAG = "periph.lcd";

#define PERIPH_LCD_DEFAULT_BRIGHTNESS 80

#define LVGL_BUFFER_LINES 10

static bool s_initialized = false;
static uint8_t s_brightness = PERIPH_LCD_DEFAULT_BRIGHTNESS;
static lv_display_t *s_disp = NULL;

/* 自己的 flush 回调：包一层错误处理。
 *
 * esp_lvgl_port 自带的实现直接调 esp_lcd_panel_draw_bitmap() 且**忽略返回值**；底层一旦失败
 * （典型场景：帧缓冲在 PSRAM 时，ESP32-S3 的 SPI 驱动认定 PSRAM 不可 DMA，会为每一笔
 * 传输临时申请一块内部 DMA 回弹缓冲，Wi-Fi / BLE 起来后这笔分配可能失败），
 * lv_display_flush_ready() 就永远不会被调用。而单缓冲下 LVGL 会在 lv_refr.c 里
 * `while(draw_buf->flushing)` 死等这块缓冲 —— 表现为 LVGL 任务占满 CPU、界面永久卡住、
 * 事件总线任务被饿死触发看门狗。
 *
 * 这里失败时补一次 lv_display_flush_ready()：最多丢一帧，绝不死锁，并把错误码打出来。
 * 成功路径不变：传输完成的回调仍由 panel IO 的 on_color_trans_done 触发。
 *
 * LVGL 9 已移除 LV_COLOR_16_SWAP（原来由它完成字节交换），所以在写屏前把 RGB565
 * 逐像素换成大端 —— 见 AGENTS.md 4.3。 */
static void periph_lcd_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint32_t px_count = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
    uint16_t *px = (uint16_t *)px_map;
    for (uint32_t i = 0; i < px_count; i++) {
        px[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = drv_st7789_get_panel_handle(&panel);

    if (err == ESP_OK) {
        err = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    }

    if (err != ESP_OK) {
        static uint32_t s_flush_err = 0;
        if (s_flush_err++ == 0) {
            ESP_LOGE(TAG, "flush failed: %s (area %d,%d-%d,%d), flush_ready forced",
                     esp_err_to_name(err), (int)area->x1, (int)area->y1,
                     (int)area->x2, (int)area->y2);
        }
        lv_display_flush_ready(disp);
    }
}

esp_err_t periph_lcd_init(void)
{
    if (s_initialized) return ESP_OK;

    /* 1. 取 Drivers 层已初始化的 panel / io handle */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(drv_st7789_get_panel_handle(&panel), TAG, "LCD panel not initialized");
    ESP_RETURN_ON_ERROR(drv_st7789_get_io_handle(&io), TAG, "LCD io not initialized");

    /* 2. LVGL port + display
     * 帧缓冲放**内部 DMA 内存**而不是 PSRAM：ESP32-S3 的 esp_ptr_dma_capable() 只覆盖内部
     * DRAM（SOC_DMA_LOW~SOC_DMA_HIGH），PSRAM 会被 SPI 驱动判为不可 DMA，于是每笔 flush
     * 每笔 flush 都要临时申请一块同样大小的内部 DMA 回弹缓冲；Wi-Fi / BLE 起来后这笔分配可能失败，
     * 导致刷屏失败（进而 LVGL 卡死，见 periph_lcd_flush_cb 的注释）。
     * 10 行缓冲 = 320*10*2 = 6.4 KB 内部内存（开机时、Wi-Fi / BLE 之前分配；内部 RAM 很紧，
     * 不要再放大）。 */
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
            .buff_dma = true,
            .buff_spiram = false,
        },
    };

    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");

    /* 用带错误处理的 flush 回调（见上） */
    lv_display_set_flush_cb(s_disp, periph_lcd_flush_cb);

    /* 默认屏幕设为深色，避免 LVGL 默认浅色主题导致白屏 */
    if (lvgl_port_lock(0)) {
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x121212), 0);
        lvgl_port_unlock();
    }

    /* 3. 点亮背光前先整屏清黑：ST7789 GRAM 掉电/复位后不会自动清空，
     *    否则背光一亮会先显示上一次运行残留在面板里的画面。
     *    此时 LVGL 任务已在跑，必须持锁，避免与 LVGL 的 flush 争用 panel IO */
    if (lvgl_port_lock(0)) {
        periph_lcd_fill(0x0000);
        lvgl_port_unlock();
    }

    /* 4. 背光：先用默认亮度点亮，Services 层会随后套用持久化的值 */
    drv_ledc_set_brightness(s_brightness);

    s_initialized = true;
    ESP_LOGI(TAG, "initialized, brightness=%d, %ux%u", s_brightness,
             DRV_LCD_H_RES, DRV_LCD_V_RES);
    return ESP_OK;
}

lv_display_t *periph_lcd_get_disp(void)
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
