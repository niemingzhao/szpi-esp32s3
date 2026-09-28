idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES drivers driver esp_lcd esp_lcd_touch esp_lvgl_port nvs_flash esp_timer freertos fatfs sdmmc spiffs
    PRIV_REQUIRES esp_lcd_touch_ft5x06
)
