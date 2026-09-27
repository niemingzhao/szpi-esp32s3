idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    # Web 管理页的页面本体：构建期打进固件（符号名按 IDF 规则只取文件名：
    # _binary_index_html_start / _end，见 svc_web.c）
    EMBED_FILES "assets/web/index.html"
    REQUIRES peripherals nvs_flash esp_netif esp_wifi esp_event esp_http_client esp_http_server esp_timer
             chmorgan__esp-libhelix-mp3 mbedtls mqtt esp_websocket_client esp_app_format bt spi_flash
)

# 崩溃现场捕获：包装 IDF 的 panic handler，实现见 svc_sysinfo.c
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# HID 协议本体里有两个常量只在开启 vendor report 时才被引用，这里把对应告警降级
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=unused-const-variable)
