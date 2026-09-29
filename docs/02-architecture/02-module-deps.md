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
│                           Peripherals                                    │
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
│   driver/i2c_master  driver/spi  driver/i2s  driver/sdmmc  driver/ledc    │
│   esp_lcd  esp_lcd_touch  esp_timer  nvs_flash  vfs  esp_event             │
│   esp_wifi  bt  esp_http_client  mqtt  lvgl  esp_lvgl_port              │
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
                └─→ fw_input ──← periph_button（BOOT 键，直接回调；见 01-layer-design 例外）

fw_asset ──→ (不依赖其他 fw，可独立)
fw_theme ──→ svc_settings
```

## 4. 启动依赖顺序

```
nvs_flash_init()
    │
    ▼
bsp_init()                   // Drivers：I2C → LEDC → PCA9557 → ST7789 → FT6336 → BOOT 键 → QMI8658
    │
    ▼
peripherals_init_all()       // Peripherals
    ├── periph_io_exp_init()        // PCA9557（LCD_CS / PA_EN / DVP_PWDN 的前提）
    ├── periph_audio_init()         // 音频
    ├── periph_lcd_init()           // LCD + LVGL display
    ├── periph_touch_init()         // 触摸（注册 LVGL input device）
    ├── periph_imu_init()           // IMU
    ├── periph_storage_init()       // TF(FAT) + SPIFFS
    ├── periph_camera_init()        // Camera
    └── periph_button_init()        // BOOT 键

    │
    ▼
services_init()              // EventBus → Storage → Time → Audio → Net → Power → Notification
    │
    ▼
fw_init()                    // Theme / Asset / Window / Input / StatusBar / CtrlCenter / NotiCenter / AppMgr
    │
    ▼
app_register_all()           // 注册所有内置 App
    │
    ▼
fw_app_mgr_launch("Home")    // 启动桌面
```

## 5. CMake 依赖声明

每个 component 的 `idf_component_register` 必须正确声明依赖：

```cmake
# drivers/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES driver esp_lcd esp_lcd_touch esp_lcd_touch_ft5x06 esp_timer freertos
)
```

```cmake
# peripherals/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES drivers driver esp_lcd esp_lcd_touch esp_lvgl_port nvs_flash esp_timer freertos fatfs sdmmc spiffs
    PRIV_REQUIRES esp_lcd_touch_ft5x06
)
```

```cmake
# services/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES peripherals esp_wifi bt nvs_flash esp_http_client
    PRIV_REQUIRES drivers freertos json
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
    SRC_DIRS app_clock app_settings app_music app_recorder app_image
              app_video app_camera app_file app_editor app_calc
              app_imu app_ble app_browser app_ota app_debug app_about app_factory
    INCLUDE_DIRS .
    REQUIRES framework services
    PRIV_REQUIRES freertos json
)
```

## 6. 关键外部组件依赖

```
lvgl/lvgl                       # GUI 库
espressif/esp_lvgl_port         # LVGL 适配层
espressif/esp_lcd_touch_ft5x06 # FT6336 驱动（I2C panel IO v2）
espressif/esp_codec_dev         # 音频 codec 抽象（录音 / 多 codec 时使用）
espressif/es7210                # 音频 ADC
espressif/esp32-camera          # GC0308 摄像头
chmorgan/esp-audio-player       # 音频播放
chmorgan/esp-file-iterator      # 文件迭代器
espressif/mdns                  # mDNS 服务
espressif/esp_websocket_client  # WebSocket
```

各组件使用各自最新稳定版本。

ST7789 / PCA9557 / FT6336 / QMI8658 / ES8311 的芯片级驱动在本仓库 `drivers/` 自实现，I2C 统一走新版 `driver/i2c_master`（经 `drv_i2c_*` 封装），不引入对应的第三方组件。