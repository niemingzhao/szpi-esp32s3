idf_component_register(
    SRC_DIRS src app_home app_clock
    INCLUDE_DIRS include app_home app_clock
    REQUIRES framework services lvgl esp_lvgl_port
    PRIV_REQUIRES freertos
)
