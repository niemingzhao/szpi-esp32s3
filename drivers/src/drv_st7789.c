/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * SZPI-OS - ST7789 LCD Panel Driver
 */

#include "drv_common.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

static const char *TAG = "drv.st7789";

#define BSP_LCD_SPI_MOSI      GPIO_NUM_40
#define BSP_LCD_SPI_CLK       GPIO_NUM_41
#define BSP_LCD_DC            GPIO_NUM_39
#define BSP_LCD_SPI_NUM       SPI3_HOST
#define BSP_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BSP_LCD_H_RES         320
#define BSP_LCD_V_RES         240

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static bool s_initialized = false;

esp_err_t drv_st7789_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

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

    // Panel IO - CS 由 PCA9557.BIT0 控制，因此不占用 GPIO
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = GPIO_NUM_NC,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 10,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_io_handle));

    // ST7789 面板
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,  // RST NC，靠软件复位
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle));

    // 软件复位
    esp_lcd_panel_reset(s_panel_handle);

    // 由 PCA9557 拉低 CS
    drv_pca9557_set_pin(DRV_PCA9557_LCD_CS, 0);

    // 初始化面板
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    // 配置 ST7789 显示参数
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    s_initialized = true;
    ESP_LOGI(TAG, "ST7789 initialized (SPI3, 80 MHz, 320×240)");
    return ESP_OK;
}

esp_err_t drv_st7789_get_panel_handle(esp_lcd_panel_handle_t *out_handle)
{
    if (s_panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_handle = s_panel_handle;
    return ESP_OK;
}

esp_err_t drv_st7789_get_io_handle(esp_lcd_panel_io_handle_t *out_io_handle)
{
    if (s_io_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_io_handle = s_io_handle;
    return ESP_OK;
}
