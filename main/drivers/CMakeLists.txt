idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_driver_spi esp_lcd esp_lcd_touch esp32-camera
    PRIV_REQUIRES esp_lcd_touch_ft5x06
)
