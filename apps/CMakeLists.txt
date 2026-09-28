idf_component_register(
    SRC_DIRS src app_home app_clock app_settings
    INCLUDE_DIRS include app_home app_clock app_settings
    REQUIRES framework services lvgl esp_lvgl_port heap esp_hw_support
    PRIV_REQUIRES freertos
)
