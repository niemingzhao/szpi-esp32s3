idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES peripherals nvs_flash esp_netif esp_wifi esp_event esp_http_client esp_http_server esp_timer freertos chmorgan__esp-libhelix-mp3 esp_https_ota mbedtls mqtt esp_websocket_client esp_hw_support heap esp_system console esp_app_format bt spi_flash
    PRIV_REQUIRES drivers
)

# 崩溃现场捕获（SYS-005）：包装 IDF 的 panic handler，实现见 svc_sysinfo.c
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# 移植的 HID 协议本体里有两个属性常量只在开启 vendor report 时才引用（上游例程的
# CMakeLists 也是加 -Wno-unused-const-variable 处理的），这里降级为告警而不是错误
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=unused-const-variable)
