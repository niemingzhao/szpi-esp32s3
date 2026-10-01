idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    # IDF v6 把驱动拆成了独立的 esp_driver_* 组件：I2S 在 esp_driver_i2s、UART 在 esp_driver_uart，
    # GPIO / LEDC / SPI / SDMMC 也从原来的 driver 组件拆了出去，用到的都要显式声明。
    REQUIRES drivers driver esp_lcd esp_lcd_touch esp_lvgl_port nvs_flash esp_timer freertos fatfs sdmmc spiffs esp_adc
             esp_driver_i2s esp_driver_uart esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_sdmmc
    PRIV_REQUIRES esp_lcd_touch_ft5x06
)
