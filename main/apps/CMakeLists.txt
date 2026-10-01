idf_component_register(
    SRC_DIRS src app_home app_scripts app_clock app_calendar app_weather app_display app_sound
             app_wifi app_bt app_download app_stopwatch app_timer app_perf app_log app_music
             app_recorder app_image app_camera app_file app_editor app_calc app_imu app_about
    INCLUDE_DIRS include app_home app_scripts app_clock app_calendar app_weather app_display
                 app_sound app_wifi app_bt app_download app_stopwatch app_timer app_perf
                 app_log app_music app_recorder app_image app_camera app_file app_editor
                 app_calc app_imu app_about
    REQUIRES framework services lvgl esp_lvgl_port heap esp_hw_support cjson
    PRIV_REQUIRES freertos
)
