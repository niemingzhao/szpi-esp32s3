idf_component_register(
    SRC_DIRS src assets
    INCLUDE_DIRS include
    REQUIRES services peripherals lvgl esp_lvgl_port
    PRIV_REQUIRES freertos
)
