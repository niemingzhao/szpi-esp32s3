idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES peripherals nvs_flash esp_netif esp_wifi esp_event esp_http_client esp_timer freertos chmorgan__esp-libhelix-mp3
    PRIV_REQUIRES drivers
)
