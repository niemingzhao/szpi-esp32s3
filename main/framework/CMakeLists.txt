idf_component_register(
    SRC_DIRS src assets
    INCLUDE_DIRS include
    REQUIRES services peripherals lvgl esp_lvgl_port lua
    PRIV_REQUIRES freertos
)
