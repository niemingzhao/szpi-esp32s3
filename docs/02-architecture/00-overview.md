# 架构总览

## 1. 设计目标

SZPI-OS 的架构满足：

| 目标 | 含义 |
|------|------|
| 流畅 UI | 触摸、状态栏、通知、应用切换响应迅速 |
| 可学习 | 代码结构清晰，每层职责明确 |
| 可扩展 | 添加新 app 简单，HAL 可替换 |
| 可裁剪 | 各层通过 menuconfig / 编译宏开关，按需编译 |
| 极客友好 | 提供调试入口、串口 shell、性能监控 |

## 2. 分层架构图

```
┌────────────────────────────────────────────────────────────────────┐
│                         Apps Layer (应用层)                          │
│   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ │
│   │  Clock   │ │  Music   │ │   File   │ │ Settings │ │   ...    │ │
│   └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘ │
│   每个 App 是独立的 FreeRTOS 任务，通过 framework API 与系统交互      │
└──────────────┬─────────────────────────────────────────────────────┘
               │  app_if.h
┌──────────────▼─────────────────────────────────────────────────────┐
│              Framework Layer (框架层)                               │
│   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ │
│   │ AppMgr   │ │  Window  │ │   Input  │ │   Theme  │ │   Asset  │ │
│   └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘ │
│   LVGL 控件封装、事件路由、动画、字体管理                            │
└──────────────┬─────────────────────────────────────────────────────┘
               │  svc_if.h
┌──────────────▼─────────────────────────────────────────────────────┐
│              Services Layer (服务层)                                 │
│   ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ │
│   │  Time  │ │ Audio  │ │  Net   │ │Storage │ │  Noti  │ │ Power  │ │
│   └────────┘ └────────┘ └────────┘ └────────┘ └────────┘ └────────┘ │
│   跨模块业务逻辑：音频编解码、网络协议栈、文件系统抽象、通知分发       │
└──────────────┬─────────────────────────────────────────────────────┘
               │  hal_if.h
┌──────────────▼─────────────────────────────────────────────────────┐
│              HAL Layer (硬件抽象层)                                 │
│   ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │
│   │  LCD │ │Touch │ │Codec │ │ IMU  │ │  TF  │ │ Cam  │ │ IO Exp│  │
│   └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘  │
│   屏蔽硬件差异，提供统一的 C 接口                                    │
└──────────────┬─────────────────────────────────────────────────────┘
               │  driver_xxx.h
┌──────────────▼─────────────────────────────────────────────────────┐
│              Drivers Layer (驱动层)                                 │
│   ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ │
│   │ BSP    │ │PCA9557 │ │ST7789  │ │FT6336  │ │QMI8658 │ │ SDMMC  │ │
│   │ Init   │ │         │ │         │ │         │ │         │ │         │ │
│   └────────┘ └────────┘ └────────┘ └────────┘ └────────┘ └────────┘ │
│   板级具体实现 + 第三方 driver 封装                                  │
└──────────────┬─────────────────────────────────────────────────────┘
               │
┌──────────────▼─────────────────────────────────────────────────────┐
│           ESP-IDF + FreeRTOS (内核层)                               │
│   NVS / VFS / LwIP / WiFi / BT / SDMMC / SPI / I2C / I2S / LVGL    │
└────────────────────────────────────────────────────────────────────┘
```

## 3. 各层职责定义

### 3.1 Drivers 层

**职责**：与 ESP-IDF 驱动直接对接的薄包装层。

**包含**：
- `bsp_init` - 板级外设初始化顺序管理器
- `pca9557` - IO 扩展芯片驱动
- `st7789` - LCD 面板驱动
- `ft6336` - 触摸芯片驱动
- `qmi8658` - IMU 驱动
- `es8311` - 音频 DAC 驱动
- `es7210` - 音频 ADC 驱动
- `gc0308` - 摄像头驱动
- `sd_card` - SDMMC TF 卡驱动
- `key_gpio0` - BOOT 按键驱动
- `led_pwm` - LEDC PWM 封装

**约束**：
- 不调用任何其他 SZPI-OS 层
- 只依赖 ESP-IDF / FreeRTOS / 第三方组件
- 输出面向 HAL 的函数

### 3.2 HAL 层

**职责**：提供面向业务的硬件抽象 API，隐藏具体芯片型号。

**包含**：
- `hal_lcd` - 显示屏
- `hal_touch` - 触摸
- `hal_audio` - 音频
- `hal_imu` - 姿态
- `hal_storage` - 存储
- `hal_camera` - 摄像头
- `hal_io_exp` - IO 扩展
- `hal_button` - 按键

**约束**：
- 只调用 Drivers 层 + ESP-IDF
- 不依赖 Services / Framework / Apps
- 提供面向业务的统一 API（如音量用 0-100）

### 3.3 Services 层

**职责**：提供跨多个 HAL 的业务服务，处理协议、状态、并发。

**包含**：
- `svc_time` - 时间管理（SNTP 同步、时区）
- `svc_audio` - 音频播放、录音、提示音
- `svc_net` - WiFi、BLE、HTTP、MQTT
- `svc_storage` - 文件系统、路径管理、应用沙箱
- `svc_notification` - 通知队列、分类、回调
- `svc_power` - 电源管理（背光超时、休眠）
- `svc_event_bus` - 事件总线（贯穿所有层）

**约束**：
- 调用 HAL 层
- 不调用 Framework / Apps
- 每个服务一个独立的 FreeRTOS 任务（必要时）

### 3.4 Framework 层

**职责**：UI 系统框架，封装 LVGL，提供 App 编程模型。

**包含**：
- `fw_app_mgr` - 应用管理：注册、生命周期、消息路由
- `fw_window` - 窗口管理：多窗口栈、过渡动画
- `fw_input` - 输入管理：手势识别、按键路由、虚拟按键
- `fw_theme` - 主题管理：深色 / 浅色主题
- `fw_asset` - 资源管理：图片 / 字体缓存
- `fw_statusbar` - 状态栏组件
- `fw_control_center` - 控制中心组件
- `fw_notification` - 通知中心组件

**约束**：
- 调用 Services 层
- 不直接调用 HAL（必须经 Services）
- 强依赖 LVGL

### 3.5 Apps 层

**职责**：独立的应用，每个 app 是一个子目录，编译进同一个 firmware。

**内置应用**（共 16 个）：
- `app_clock` - 时钟
- `app_settings` - 设置
- `app_music` - 音乐播放器
- `app_recorder` - 录音机
- `app_image` - 图片查看器
- `app_video` - 视频播放器
- `app_camera` - 相机
- `app_file` - 文件管理器
- `app_editor` - 文本编辑器
- `app_calc` - 计算器
- `app_imu` - 姿态传感器显示
- `app_ble` - 蓝牙 HID 模拟器
- `app_browser` - 浏览器
- `app_ota` - OTA 升级
- `app_debug` - 调试控制台
- `app_about` - 关于本机
- `app_factory` - 工厂模式

**约束**：
- 调用 Framework + Services 层
- 不直接调用 HAL
- 通过 `fw_app_mgr_register` 注册

### 3.6 Main（`main/`）

**职责**：系统入口、引导序列、默认任务。

**包含**：
- `main.c` - app_main() 函数
- `app_register.c` - 所有内置 app 的注册表

## 4. 关键技术选型

| 技术 | 选择 | 用途 |
|------|------|------|
| 操作系统 | FreeRTOS | ESP-IDF 内置 |
| GUI | LVGL | 嵌入式 GUI |
| LVGL 适配 | esp_lvgl_port | 触摸适配、线程安全 |
| 文件系统 | FAT32 (TF) + LittleFS (内置 Flash) | 双 FS |
| 持久化 | NVS | 系统配置、WiFi 凭据 |
| 音频解码 | helix MP3 | MP3 解码 |
| 蓝牙 | NimBLE | BLE 主机 / 外设 |
| HTTP | esp_http_client | HTTP 客户端 |
| MQTT | esp-mqtt | MQTT 客户端 |
| WebSocket | esp_websocket_client | WebSocket 客户端 |
| OTA | esp_app_update | OTA 升级 |

## 5. 数据流

### 5.1 触摸事件流

```
FT6336 中断 / 轮询
  → drv_ft6336_read()
    → hal_touch_get_xy()
      → fw_input_dispatch()
        → 当前激活 app 的 LVGL input device 回调
        → 或全局手势回调（返回、HOME）
```

### 5.2 音频播放流

```
App 调用 svc_audio_play(path)
  → svc_audio 任务接收请求
    → svc_storage 读取文件
    → helix MP3 解码
    → I2S DMA buffer
      → ES8311
        → NS4150B 功放
          → 喇叭
```

### 5.3 通知流

```
App 调用 svc_notification_send(noti)
  → svc_notification 队列
    → 触发声音提示（svc_audio）
    → 更新通知中心 UI（fw_notification）
      → 用户点击 → 跳转到目标 app
```

### 5.4 配置持久化流

```
App 调用 svc_settings_set(key, value)
  → NVS write
  → 触发变更事件
    → fw_app_mgr_broadcast(KEY_CHANGED)
      → 关心此 key 的 app 收到通知并刷新 UI
```

## 6. 任务规划

### 6.1 系统任务清单

| 任务名 | 优先级 | 核心 | 职责 |
|--------|--------|------|------|
| `main_task` | 1 | 0 | 启动序列 |
| `lvgl_task` | 5 | - (LVGL port) | LVGL tick + render |
| `input_task` | 4 | 0 | 触摸扫描、按键扫描 |
| `imu_task` | 3 | 0 | 周期性读取 IMU 数据 |
| `audio_play_task` | 6 | 1 | 音频解码 + 写入 I2S |
| `audio_feed_task` | 6 | 1 | 麦克风采集 |
| `net_event_task` | 4 | 0 | WiFi 事件处理 |
| `ntp_sync_task` | 2 | 0 | SNTP 周期同步 |
| `power_task` | 2 | 0 | 背光超时、休眠 |
| `sd_monitor_task` | 3 | 0 | SD 卡热插拔监测 |
| `app_<name>_task` | 各异 | 各异 | 应用私有任务 |

### 6.2 任务间通信

- **LVGL 操作**：必须经 `lvgl_port_lock(0) / unlock()`
- **跨任务消息**：用 FreeRTOS Queue 或事件组
- **全局状态**：用互斥锁保护的全局结构体
- **服务调用**：阻塞式 API（带 timeout）

## 7. 启动序列

```
1. nvs_flash_init()
2. Drivers 初始化（I2C, SPI, LEDC, PCA9557）
3. HAL 初始化（LCD, Touch, IMU, Codec, Storage, Camera, Button, IO）
4. LVGL port 初始化
5. 注册 LVGL display + input device
6. Services 启动（EventBus, Time, Audio, Net, Storage, Notification, Power）
7. Framework 初始化（Theme, Asset, Window, AppMgr）
8. Apps 注册
9. 显示启动 logo 动画
10. 启动桌面
11. 创建 LVGL 任务
12. 进入事件循环
```

## 8. 内存预算

| 区域 | 大小 | 用途 |
|------|------|------|
| 内置 SRAM | 512 KB | FreeRTOS 任务栈、LVGL 控制块、关键 buffer |
| PSRAM | 8 MB | LVGL framebuffer、字体、图片、视频缓冲 |
| Flash App (factory + ota_0) | 各 4 MB | 用户应用、字库、图片素材 |
| Flash App (ota_1) | 4 MB | OTA 备份 |
| Flash FS (storage) | 3 MB | LittleFS：配置、脚本、用户数据 |

**关键原则**：
- LVGL framebuffer 强制放在 PSRAM
- 所有非关键 buffer 优先 PSRAM
- 关键实时路径（音频解码、触摸）放内置 SRAM

## 9. 目录结构

```
szpi-esp32s3/
├── CMakeLists.txt              # 顶层
├── partitions.csv              # 分区表
├── sdkconfig.defaults          # 默认配置
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  # app_main()
│   └── app_register.c          # app 注册表
├── drivers/                    # Drivers 层
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
├── hal/                        # HAL 层
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
├── services/                   # Services 层
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
├── framework/                  # Framework 层
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
├── apps/                       # Apps 层
│   ├── CMakeLists.txt
│   ├── app_clock/
│   ├── app_music/
│   ├── app_settings/
│   └── .../
├── components/                 # 第三方 ESP-IDF 组件依赖
│   ├── lvgl/
│   ├── esp_lvgl_port/
│   └── ...
└── docs/
    ├── 01-requirements/
    ├── 02-architecture/
    └── 03-design/
```

## 10. 架构类比

| SZPI-OS | 对应概念 |
|---------|----------|
| Apps | 应用 |
| Framework | 窗口管理 + 控件系统 + 资源管理 |
| Services | 系统服务（音频 / 网络 / 存储） |
| HAL | 硬件抽象层 |
| Drivers | 内核驱动 |
| Main (app_main) | 系统启动器 |

## 11. 关键技术决策

- **5 层架构**：每层职责独立，互不逆向调用
- **服务总线**：跨层通信统一通过 svc_event_bus
- **静态库组织**：每层编译为静态库，由 main 链接
- **LVGL 封装**：所有 UI 控件封装在 Framework 层
- **应用隔离**：每个 app 独立 NVS 命名空间 + 独立目录