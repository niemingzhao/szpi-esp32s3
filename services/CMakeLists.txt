idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES peripherals nvs_flash esp_netif esp_timer freertos
    PRIV_REQUIRES drivers
)
