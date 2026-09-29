idf_component_register(
    SRC_DIRS src app_home app_clock app_settings app_ble
    INCLUDE_DIRS include app_home app_clock app_settings app_ble
    REQUIRES framework services lvgl esp_lvgl_port heap esp_hw_support
    PRIV_REQUIRES freertos
)
