# 模块依赖图

## 1. 模块全景

```
┌──────────────────────────────────────────────────────────────────────┐
│                               Apps / 脚本                              │
│   ┌──────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌──────┐  │
│   │Script│ │Clk │ │Cal │ │Wthr│ │File│ │Mus │ │Cam │ │Perf│ │ ...  │  │
│   └───┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └──┬───┘  │
└───────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼───────┼──────┘
        │      │      │      │      │      │      │      │       │
        ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼       ▼
┌──────────────────────────────────────────────────────────────────────┐
│                              Framework                                │
│   ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐      │
│   │AppMgr│ │Script│ │Window│ │Input │ │Theme │ │Asset │ │  UI  │      │
│   └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘      │
│       │        │        │        │        │        │        │         │
└───────┼────────┼────────┼────────┼────────┼────────┼────────┼─────────┘
        │        │        │        │        │        │        │
        ▼        ▼        ▼        ▼        ▼        ▼        ▼
┌──────────────────────────────────────────────────────────────────────┐
│                               Services                                │
│   ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐      │
│   │Event │ │Storag│ │ Time │ │Audio │ │ Net  │ │  BT  │ │  IO  │      │
│   │ Bus  │ │  e   │ │      │ │      │ │      │ │      │ │      │      │
│   └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘ └───┬──┘      │
└───────┼────────┼────────┼────────┼────────┼────────┼────────┼─────────┘
        │        │        │        │        │        │        │
        ▼        ▼        ▼        ▼        ▼        ▼        ▼
┌──────────────────────────────────────────────────────────────────────┐
│                             Peripherals                               │
│   ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐      │
│   │LCD │ │Tch │ │Aud │ │IMU │ │Stor│ │Cam │ │ IEx│ │ Btn│ │Ext │      │
│   └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘ └─┬──┘      │
└─────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼──────┼─────────┘
      │      │      │      │      │      │      │      │      │
      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼      ▼
┌──────────────────────────────────────────────────────────────────────┐
│                               Drivers                                 │
│   ┌────┐ ┌────┐ ┌─────┐ ┌────┐ ┌────┐ ┌─────┐ ┌─────┐ ┌──────┐       │
│   │BSP │ │I2C │ │PCA95│ │ST77│ │FT63│ │QMI86│ │ES83 │ │Camera│  ...  │
│   │    │ │    │ │ 57  │ │ 89 │ │ 36 │ │ 58  │ │ /ES72│ │      │       │
│   └────┘ └────┘ └─────┘ └────┘ └────┘ └─────┘ └─────┘ └──────┘       │
└───────────────────────────────────┬──────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│                      ESP-IDF + FreeRTOS + 第三方组件                   │
│   driver/i2c_master  driver/spi  driver/i2s  driver/sdmmc  driver/ledc │
│   esp_lcd  esp_lcd_touch  esp_timer  nvs_flash  vfs  esp_event         │
│   esp_wifi  bt  esp_http_client  lvgl  esp_lvgl_port  lua  …           │
└──────────────────────────────────────────────────────────────────────┘
```

## 2. Services 依赖细节

```
                     ┌──────────────────────┐
                     │    svc_event_bus     │ 核心，被所有其他服务依赖
                     └──────────┬───────────┘
                                │
        ┌───────────┬───────────┼───────────┬───────────┬───────────┐
        ▼           ▼           ▼           ▼           ▼           ▼
    svc_time   svc_audio   svc_storage   svc_net    svc_power    svc_imu
        │           │           │           │           │           │
        │           └─ svc_time ◄┤           │           │           │
        │                       │           │           │           │
        └─ svc_storage ◄────────┘           │           │           │
                                            └─ svc_settings ◄────────┤
                                                                      │
    svc_bt / svc_mqtt / svc_ws / svc_io / svc_camera / svc_sysinfo ───┘
```

- `svc_settings` 是最底层服务，几乎所有服务都读它
- `svc_net` 通知 `svc_time` 触发 SNTP
- `svc_audio`、`svc_camera` 经 `svc_storage` 读写文件

## 3. Framework 依赖细节

```
fw_app_mgr ──→ fw_window ──→ fw_theme ──→ svc_settings
    │
    ├─→ fw_statusbar ──→ svc_time, svc_net, svc_bt
    ├─→ fw_ui ──→ fw_theme, fw_asset
    ├─→ fw_input ──← periph_button（BOOT 键，直接回调，见 01-layer-design 例外）
    └─→ fw_script ──→ fw_ui, fw_input（界面 / 输入 / 通知）
                  └─→ svc_audio, svc_camera, svc_storage, svc_net, svc_bt,
                      svc_time, svc_sysinfo（系统能力）
                  └─→ svc_io（GPIO / PWM / I2C / UART / ADC）

fw_asset ──→ （不依赖其他 fw）
fw_boot_animation ──→ fw_asset, svc_audio
```

## 4. 脚本子系统依赖

```
fw_script
  ├── Lua 5.4 运行时（espressif/lua）
  ├── 脚本目录扫描 / 元信息 / 示例脚本释放 / 网络下载
  ├── 界面 / 输入 / 通知绑定 ──→ fw_ui, fw_input
  ├── 系统能力绑定 ──→ svc_audio, svc_camera, svc_storage, svc_net,
  │                     svc_bt, svc_time, svc_sysinfo, svc_event_bus
  └── 硬件绑定 ──→ svc_io ──→ periph_ext
```

脚本与原生 App 共用 `svc_event_bus`、`svc_settings`、`svc_storage`。

## 5. 启动依赖顺序

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
    ├── periph_storage_init()       // TF 卡挂载
    ├── periph_button_init()        // BOOT 键
    └── periph_ext_init()           // 外扩接口

    │
    ▼
services_init()              // Watchdog → EventBus → Settings → Storage → BT → Time →
                             //   Audio → Net → Power → IMU → IO → Camera → SysInfo
    │
    ▼
fw_init()                    // Theme / Asset / Window / AppMgr / Script / UI / StatusBar / Input
    │
    ▼
app_register_all()           // 注册所有内置 App
    │
    ▼
fw_boot_animation()          // 开机画面 + 提示音
    │
    ▼
fw_app_mgr_launch("Home")    // 进入桌面
    │
    ▼
periph_storage_mount(内置 Flash) → svc_watchdog_arm()
```

## 6. CMake 依赖声明

```cmake
# main/drivers/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES driver esp_driver_i2c esp_lcd esp_lcd_touch esp_lcd_touch_ft5x06 esp_timer freertos esp32-camera
)
```

```cmake
# main/peripherals/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES drivers driver esp_lcd esp_lcd_touch esp_lvgl_port nvs_flash esp_timer freertos fatfs sdmmc spiffs
    PRIV_REQUIRES esp_lcd_touch_ft5x06
)
```

```cmake
# main/services/CMakeLists.txt
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES peripherals nvs_flash esp_netif esp_wifi esp_event esp_http_client esp_http_server
             esp_timer freertos chmorgan__esp-libhelix-mp3 mbedtls esp_websocket_client
             esp_hw_support heap esp_system esp_app_format bt spi_flash mqtt
    PRIV_REQUIRES drivers
)
```

```cmake
# main/framework/CMakeLists.txt
idf_component_register(
    SRC_DIRS src assets
    INCLUDE_DIRS include
    REQUIRES services lvgl esp_lvgl_port lua
    PRIV_REQUIRES freertos
)
```

```cmake
# main/apps/CMakeLists.txt（新 App 落地时把目录加进 SRC_DIRS / INCLUDE_DIRS）
idf_component_register(
    SRC_DIRS src app_scripts app_clock app_calendar app_weather app_wifi app_bt
             app_display app_sound app_file app_editor app_calc app_download
             app_music app_recorder app_camera app_image app_stopwatch app_timer
             app_imu app_perf app_log app_about
    INCLUDE_DIRS include app_scripts app_clock app_calendar app_weather app_wifi app_bt
                 app_display app_sound app_file app_editor app_calc app_download
                 app_music app_recorder app_camera app_image app_stopwatch app_timer
                 app_imu app_perf app_log app_about
    REQUIRES framework services lvgl esp_lvgl_port heap esp_hw_support
    PRIV_REQUIRES freertos
)
```

## 7. 关键外部组件依赖

```
lvgl/lvgl                       # GUI 库
espressif/esp_lvgl_port         # LVGL 适配层
espressif/esp_lcd_touch_ft5x06  # FT6336 触摸（I2C panel IO v2）
espressif/esp32-camera          # GC0308 / GC2145 摄像头
espressif/lua                   # Lua 5.4 运行时（脚本）
chmorgan/esp-libhelix-mp3       # MP3 解码
espressif/esp_websocket_client  # WebSocket 客户端
espressif/mqtt                  # MQTT 客户端（IDF v6 起为独立组件）
espressif/json                  # JSON（IDF v6 起为独立组件）
```

各组件使用各自最新稳定版本。

ST7789 / PCA9557 / FT6336 / QMI8658 / ES8311 / ES7210 的芯片级驱动在本仓库 `main/drivers/` 自实现，I2C 统一走新版 `driver/i2c_master`（经 `drv_i2c_*` 封装），不引入对应的第三方组件。ESP-IDF v6.1 已移除旧版 I2C 驱动，本项目统一用新版 i2c_master。
