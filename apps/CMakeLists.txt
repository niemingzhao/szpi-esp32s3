idf_component_register(
    SRC_DIRS src app_home app_clock app_settings app_ble app_music app_recorder app_image
             app_video app_camera app_file app_editor app_calc app_imu app_browser
             app_ota app_debug app_about app_factory
    INCLUDE_DIRS include app_home app_clock app_settings app_ble app_music app_recorder app_image
                 app_video app_camera app_file app_editor app_calc app_imu app_browser
                 app_ota app_debug app_about app_factory
    REQUIRES framework services lvgl esp_lvgl_port heap esp_hw_support
    PRIV_REQUIRES freertos
)
