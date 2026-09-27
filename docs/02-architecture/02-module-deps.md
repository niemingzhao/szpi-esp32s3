# 模块依赖图

## 1. 模块全景

```
┌──────────────────────────────────────────────────────────────────────────┐
│                                Apps                                       │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐  │
│  │Clk │ │Mus │ │Set │ │Fil │ │Cam │ │Rcd │ │Cfm │ │Abt │ │ ...│ │ ...│  │
│  └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └────┘ └────┘  │
└────┼─────┼─────┼─────┼─────┼─────┼─────┼─────┼───────────────────────────┘
     │     │     │     │     │     │     │     │
     ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                             Framework                                    │
│   ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐          │
│   │AppMgr│ │Window│ │Input │ │Theme │ │Asset │ │SB    │ │NC    │          │
│   └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘          │
│      │       │        │        │        │        │        │              │
└──────┼───────┼────────┼────────┼────────┼────────┼────────┼──────────────┘
       │       │        │        │        │        │        │
       ▼       ▼        ▼        ▼        ▼        ▼        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                              Services                                     │
│   ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐          │
│   │Event │ │ Time │ │ Audio│ │ Net  │ │Storag│ │ Noti │ │ Power│          │
│   │ Bus  │ │      │ │      │ │      │ │      │ │      │ │      │          │
│   └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘          │
│      │        │        │        │        │        │        │              │
└──────┼────────┼────────┼────────┼────────┼────────┼────────┼──────────────┘
       │        │        │        │        │        │        │
       ▼        ▼        ▼        ▼        ▼        ▼        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                                HAL                                        │
│   ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐                  │
│   │LCD │ │Tch │ │Aud │ │IMU │ │Stor│ │Cam │ │ IEx│ │ Btn│                  │
│   └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘                  │
└─────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼─────────────────────┘
      │      │      │      │      │      │      │      │
      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                              Drivers                                      │
│   ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐         │
│   │BSP │ │PCA │ │ST77│ │FT63│ │QMI │ │ES83│ │ES72│ │SDMMC│ │KEY │         │
│   │Init│ │9557│ │89  │ │36  │ │8658│ │11  │ │10  │ │    │ │    │         │
│   └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘         │
└────────────────────────────────────┬─────────────────────────────────────┘
                                     │
                                     ▼
┌──────────────────────────────────────────────────────────────────────────┐
│                       ESP-IDF + FreeRTOS + Third-Party                    │
│   driver/i2c  driver/spi  driver/i2s  driver/sdmmc  driver/ledc           │
│   esp_lcd  esp_lcd_touch  esp_timer  nvs_flash  esp_vfs  esp_event         │
│   esp_wifi  esp_bt  esp_http_client  esp_mqtt  lvgl  esp_lvgl_port         │
└──────────────────────────────────────────────────────────────────────────┘
```

## 2. Services 模块依赖细节

```
                     ┌──────────────────────┐
                     │    svc_event_bus     │ 核心，被所有其他服务依赖
                     └──────────┬───────────┘
                                │
        ┌───────────┬───────────┼───────────┬───────────┬───────────┐
        ▼           ▼           ▼           ▼           ▼           ▼
    svc_time    svc_audio   svc_storage   svc_net    svc_noti    svc_power
        │           │           │           │           │           │
        │           │           │           │           │           │
        │           ├─svc_time ◄┤           │           │           │
        │           │           │           │           │           │
        └─svc_storage ◄─────────────────────┘           │           │
        │                                               │           │
        └─svc_event_bus ───────────────────────────────────────────►─┘
```

## 3. Framework 模块依赖细节

```
fw_app_mgr ──→ fw_window ──→ fw_theme
                │
                ├─→ fw_statusbar ──→ svc_time
                ├─→ fw_control_center ──→ svc_net, svc_power, svc_audio
                ├─→ fw_notification ──→ svc_notification
                └─→ fw_input ──→ hal_touch (经 svc_event_bus)

fw_asset ──→ (不依赖其他 fw，可独立)
fw_theme ──→ svc_settings
```

## 4. 启动依赖顺序

```
bsp_init()                  // Drivers 初始化底层
    │
    ├── drv_pca9557_init()         // IO 扩展（最早，LCD_CS 需要它）
    ├── hal_audio_init()           // Codec（PCA9557 之后，PA_EN 需要它）
    ├── hal_lcd_init()             // LCD
    ├── hal_touch_init()           // Touch
    ├── hal_imu_init()             // IMU
    ├── hal_storage_init()         // TF + LittleFS
    ├── hal_camera_init()          // Camera (可选)
    └── hal_button_init()          // BOOT key

    │
    ▼
svc_event_bus_init()         // 必须最先，其他服务都依赖它
svc_storage_init()           // 后续 app 都要用文件系统
svc_time_init()              // 后续服务依赖时间
svc_audio_init()             // 异步启动后台任务
svc_net_init()               // 异步启动 WiFi 状态机
svc_power_init()             // 监测背光
svc_notification_init()      // 监听事件，注册到 UI

    │
    ▼
fw_init()                    // 初始化 LVGL（已由 lvgl_port 完成）
fw_theme_init()
fw_asset_init()
fw_window_init()
fw_input_init()
fw_statusbar_create()
fw_control_center_create()
fw_notification_create()
fw_app_mgr_init()

    │
    ▼
app_register_all()           // 注册所有内置 app

    │
    ▼
fw_app_mgr_launch("Home")    // 启动桌面（Home 是个特殊 app）
```

## 5. CMake 依赖声明

每个 component 的 `idf_component_register` 必须正确声明依赖：

```cmake
# drivers/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES driver esp_lcd esp_timer freertos
)
```

```cmake
# hal/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES drivers driver esp_lcd esp_lcd_touch driver esp_codec_dev
    PRIV_REQUIRES freertos esp_timer
)
```

```cmake
# services/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES hal esp_wifi esp_bt nvs_flash esp_http_client
    PRIV_REQUIRES drivers freertos cJSON
)
```

```cmake
# framework/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES services lvgl esp_lvgl_port
    PRIV_REQUIRES freertos
)
```

```cmake
# apps/CMakeLists.txt
idf_component_register(
    SRC_DIRS app_clock app_music app_settings app_file_browser
              app_calculator app_camera app_recorder app_about
    INCLUDE_DIRS .
    REQUIRES framework services
    PRIV_REQUIRES freertos cJSON
)
```

## 6. 关键外部组件依赖

```
lvgl/lvgl                       # GUI 库
espressif/esp_lvgl_port         # LVGL 适配层
espressif/esp_lcd_touch_ft5x06 # FT6336 驱动
espressif/esp_codec_dev         # 音频 codec 抽象
espressif/es8311                # 音频 DAC
espressif/es7210                # 音频 ADC
espressif/esp32-camera          # GC0308 摄像头
chmorgan/esp-audio-player       # 音频播放
chmorgan/esp-file-iterator      # 文件迭代器
espressif/mdns                  # mDNS 服务
espressif/esp_websocket_client  # WebSocket
espressif/cJSON                 # JSON 解析
```

各组件使用各自最新稳定版本。