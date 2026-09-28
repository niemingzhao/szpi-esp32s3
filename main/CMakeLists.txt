idf_component_register(SRCS "main.c"
                    REQUIRES drivers peripherals nvs_flash esp_lcd esp_lvgl_port driver
                    PRIV_REQUIRES spi_flash)
