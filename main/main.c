/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS 骨架启动
 * 第一版最小可运行版：点亮屏幕 + 显示一个按钮
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "lvgl.h"

static const char *TAG = "szpi-os";

/* ============ 板级硬件配置（基于硬件规格文档） ============ */

// I2C 总线
#define BSP_I2C_SDA           GPIO_NUM_1
#define BSP_I2C_SCL           GPIO_NUM_2
#define BSP_I2C_NUM           0
#define BSP_I2C_FREQ_HZ       100000

// I2C 从设备地址
#define PCA9557_ADDR          0x19
#define FT6336_ADDR          0x38

// PCA9557 输出位
#define LCD_CS_GPIO           BIT(0)
#define PA_EN_GPIO            BIT(1)
#define DVP_PWDN_GPIO         BIT(2)

// LCD (ST7789 / SPI)
#define BSP_LCD_SPI_MOSI      GPIO_NUM_40
#define BSP_LCD_SPI_CLK       GPIO_NUM_41
#define BSP_LCD_DC            GPIO_NUM_39
#define BSP_LCD_RST           GPIO_NUM_NC
#define BSP_LCD_BACKLIGHT     GPIO_NUM_42
#define BSP_LCD_SPI_NUM       SPI3_HOST
#define BSP_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BSP_LCD_H_RES         320
#define BSP_LCD_V_RES         240

// LEDC 背光
#define LCD_LEDC_CH           LEDC_CHANNEL_0
#define LCD_LEDC_TIMER        LEDC_TIMER_0
#define LCD_LEDC_MODE         LEDC_LOW_SPEED_MODE

/* ============ 静态变量 ============ */

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io_handle = NULL;  // LCD panel IO handle, needed by lvgl_port
static esp_lcd_touch_handle_t s_touch_handle = NULL;

/* ============ 板级初始化 ============ */

// I2C 初始化
static esp_err_t bsp_i2c_init(void)
{
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BSP_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(BSP_I2C_NUM, &i2c_conf));
    return i2c_driver_install(BSP_I2C_NUM, i2c_conf.mode, 0, 0, 0);
}

// 读 PCA9557 寄存器
static esp_err_t pca9557_read_reg(uint8_t reg, uint8_t *data)
{
    return i2c_master_write_read_device(BSP_I2C_NUM, PCA9557_ADDR, &reg, 1, data, 1, 1000 / portTICK_PERIOD_MS);
}

// 写 PCA9557 寄存器
static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = { reg, data };
    return i2c_master_write_to_device(BSP_I2C_NUM, PCA9557_ADDR, buf, sizeof(buf), 1000 / portTICK_PERIOD_MS);
}

// 设置 PCA9557 单个输出位
static esp_err_t pca9557_set_output(uint8_t gpio_bit, uint8_t level)
{
    uint8_t data;
    ESP_ERROR_CHECK(pca9557_read_reg(0x01, &data));  // OUTPUT_PORT
    if (level) {
        data |= gpio_bit;
    } else {
        data &= ~gpio_bit;
    }
    return pca9557_write_reg(0x01, data);
}

// PCA9557 初始化
static esp_err_t pca9557_init(void)
{
    // BIT0/BIT1/BIT2 设为输出，其他保持输入
    ESP_ERROR_CHECK(pca9557_write_reg(0x03, 0xF8));  // CONFIGURATION_PORT
    // 默认值: DVP_PWDN=1(掉电), LCD_CS=1(未选中), PA_EN=0(关闭)
    ESP_ERROR_CHECK(pca9557_write_reg(0x01, 0x05));
    return ESP_OK;
}

// 背光 LEDC 初始化
static esp_err_t bsp_backlight_init(void)
{
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LCD_LEDC_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    const ledc_timer_config_t timer = {
        .speed_mode = LCD_LEDC_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    return ESP_OK;
}

// 设置背光亮度 0-100
static esp_err_t bsp_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (1023 * percent) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(LCD_LEDC_MODE, LCD_LEDC_CH, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_LEDC_MODE, LCD_LEDC_CH));
    return ESP_OK;
}

// LCD 初始化
static esp_err_t bsp_lcd_init(void)
{
    ESP_ERROR_CHECK(bsp_backlight_init());

    // SPI 总线
    const spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_SPI_CLK,
        .mosi_io_num = BSP_LCD_SPI_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO));

    // Panel IO
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = GPIO_NUM_NC,  // 由 PCA9557 控制
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_lcd_io_handle));

    // ST7789 面板
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_lcd_io_handle, &panel_config, &s_panel_handle));

    esp_lcd_panel_reset(s_panel_handle);
    ESP_ERROR_CHECK(pca9557_set_output(LCD_CS_GPIO, 0));  // 拉低 CS
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    return ESP_OK;
}

// 触摸初始化（FT6336）
static esp_err_t bsp_touch_init(void)
{
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    // 注意: legacy v1 API 不需要也不能设置 scl_speed_hz

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM, &tp_io_config, &tp_io_handle));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    return esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch_handle);
}

// LVGL 启动
static esp_err_t bsp_lvgl_init(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io_handle,
        .panel_handle = s_panel_handle,
        .buffer_size = BSP_LCD_H_RES * 20,
        .double_buffer = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
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

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = s_touch_handle,
    };
    lvgl_port_add_touch(&touch_cfg);

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

    // 设置深色主题背景
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

    // 按钮上的标签
    s_label = lv_label_create(btn);
    lv_label_set_text(s_label, "Tap me!");
    lv_obj_center(s_label);

    // 底部状态
    lv_obj_t *status = lv_label_create(lv_scr_act());
    lv_label_set_text(status, "v0.1 - Skeleton boot");
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

    // 2. I2C 初始化
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_LOGI(TAG, "I2C initialized");

    // 3. PCA9557 初始化
    ESP_ERROR_CHECK(pca9557_init());
    ESP_LOGI(TAG, "PCA9557 initialized");

    // 4. LCD 初始化
    ESP_ERROR_CHECK(bsp_lcd_init());
    ESP_LOGI(TAG, "LCD initialized");

    // 5. 触摸初始化
    ESP_ERROR_CHECK(bsp_touch_init());
    ESP_LOGI(TAG, "Touch initialized");

    // 6. LVGL 初始化
    ESP_ERROR_CHECK(bsp_lvgl_init());
    ESP_LOGI(TAG, "LVGL initialized");

    // 7. 打开背光
    bsp_backlight_set(80);
    ESP_LOGI(TAG, "Backlight on (80%%)");

    // 8. 创建 UI
    create_ui();
    ESP_LOGI(TAG, "UI created");
    ESP_LOGI(TAG, "===== Ready =====");
}