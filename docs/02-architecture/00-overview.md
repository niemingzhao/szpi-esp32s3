# 架构总览

## 1. 设计目标

SZPI-OS 的架构满足：

| 目标 | 含义 |
|------|------|
| 流畅 UI | 触摸、状态栏、通知、应用切换响应迅速 |
| 可学习 | 代码结构清晰，每层职责明确 |
| 可扩展 | 添加新 App 简单，Peripherals 可替换 |
| 可裁剪 | 各层通过 menuconfig / 编译宏开关，按需编译 |
| 极客友好 | 提供调试入口、串口 shell、性能监控 |

## 2. 分层架构图

```
┌────────────────────────────────────────────────────────────────────┐
│                         Apps Layer (应用层)                          │
│   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐ │
│   │  Clock   │ │  Music   │ │   File   │ │ Settings │ │   ...    │ │
│   └──────────┘ └──────────┘ └──────────┘ └──────────┘ └──────────┘ │
│   App 通过 framework API 与系统交互，生命周期由 fw_app_mgr 管理        │
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
               │  periph_if.h
┌──────────────▼─────────────────────────────────────────────────────┐
│              Peripherals Layer (外设抽象层)                         │
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
│   NVS / VFS / LwIP / Wi-Fi / BT / SDMMC / SPI / I2C / I2S / LVGL   │
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
- `i2c` - I2C0 主机总线（新版 i2c_master 驱动）+ 寄存器读写封装
- `es8311` - 音频 DAC 驱动
- `es7210` - 音频 ADC 驱动
- `gc0308` - 摄像头驱动
- `key_gpio0` - BOOT 按键驱动
- `led_pwm` - LEDC PWM 封装

**约束**：
- 不调用任何其他 SZPI-OS 层
- 只依赖 ESP-IDF / FreeRTOS / 第三方组件
- 输出面向 Peripherals 的函数

### 3.2 Peripherals 层

**职责**：提供面向业务的外设抽象 API，隐藏具体芯片型号。

**包含**：
- `periph_lcd` - 显示屏
- `periph_touch` - 触摸
- `periph_audio` - 音频
- `periph_imu` - 姿态
- `periph_storage` - 存储
- `periph_camera` - 摄像头
- `periph_io_exp` - IO 扩展
- `periph_button` - 按键

**约束**：
- 只调用 Drivers 层 + ESP-IDF
- 不依赖 Services / Framework / Apps
- 提供面向业务的统一 API（如音量用 0-100）

### 3.3 Services 层

**职责**：提供跨多个 Peripherals 的业务服务，处理协议、状态、并发。

**包含**：
- `svc_event_bus` - 事件总线（贯穿所有层）
- `svc_settings` - 配置持久化（NVS，含 blob）
- `svc_storage` - 文件系统、路径管理、应用沙箱
- `svc_time` - 时间管理（SNTP 同步、时区）
- `svc_audio` - 音频播放、录音、提示音
- `svc_net` - Wi-Fi 状态机、HTTP 客户端、SmartConfig / AP 配网
- `svc_bt` - 蓝牙 BLE（广播、GATT 从机、扫描）
- `svc_mqtt` - MQTT 客户端
- `svc_ws` - WebSocket 客户端
- `svc_ota` - HTTPS OTA 升级
- `svc_power` - 电源管理（背光超时、休眠、唤醒）
- `svc_imu` - IMU 运动 / 姿态
- `svc_watchdog` - Task Watchdog（喂狗超时自动重启）
- `svc_notification` - 通知队列、分类、回调
- `svc_sysinfo` - 系统信息（内存、复位原因、运行时间）
- `svc_shell` - 串口命令行

**约束**：
- 调用 Peripherals 层
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
- 不直接调用 Peripherals（必须经 Services）
- 强依赖 LVGL

### 3.5 Apps 层

**职责**：独立的应用，每个 App 是一个子目录，编译进同一个 firmware。

**内置应用**（共 17 个）：
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
- 不直接调用 Peripherals
- 通过 `fw_app_mgr_register` 注册

### 3.6 Main（`main/`）

**职责**：系统入口、引导序列、默认任务。

**包含**：
- `main.c` - app_main() 函数
- `app_register.c` - 所有内置 App 的注册表

## 4. 关键技术选型

| 技术 | 选择 | 用途 |
|------|------|------|
| 操作系统 | FreeRTOS | ESP-IDF 内置 |
| GUI | LVGL | 嵌入式 GUI |
| LVGL 适配 | esp_lvgl_port | 触摸适配、线程安全 |
| 文件系统 | FAT32 (TF) + SPIFFS (内置 Flash) | 双 FS |
| 持久化 | NVS | 系统配置、Wi-Fi 凭据 |
| 音频解码 | helix MP3 | MP3 解码 |
| 蓝牙 | Bluedroid | BLE 主机 / 外设 |
| HTTP | esp_http_client | HTTP 客户端 |
| MQTT | mqtt | MQTT 客户端 |
| WebSocket | esp_websocket_client | WebSocket 客户端 |
| OTA | esp_https_ota | OTA 升级（HTTPS） |

## 5. 数据流

### 5.1 触摸事件流

```
FT6336 轮询（单点）
  → periph_touch 扫描任务：缓存坐标 + 手势识别
    → LVGL input device（read_cb 读取缓存）
      → 当前激活 App 的控件回调
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
App 调用 svc_notification_post(noti)
  → svc_notification 队列
    → 触发声音提示（svc_audio）
    → 更新通知中心 UI（fw_notification）
      → 用户点击 → 跳转到目标 App
```

### 5.4 配置持久化流

```
App 调用 svc_settings_set(key, value)
  → NVS write
  → 触发变更事件
    → fw_app_mgr_broadcast(KEY_CHANGED)
      → 关心此 key 的 App 收到通知并刷新 UI
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
| `net_event_task` | 4 | 0 | Wi-Fi 事件处理 |
| `ntp_sync_task` | 2 | 0 | SNTP 周期同步 |
| `power_task` | 2 | 0 | 背光超时、休眠 |
| `sd_monitor_task` | 3 | 0 | TF 卡热插拔监测 |
| `app_<name>_task` | 各异 | 各异 | 应用私有任务（可选） |

### 6.2 任务间通信

- **LVGL 操作**：必须经 `lvgl_port_lock(0) / unlock()`
- **跨任务消息**：用 FreeRTOS Queue 或事件组
- **全局状态**：用互斥锁保护的全局结构体
- **服务调用**：阻塞式 API（带 timeout）

## 7. 启动序列

```
1. nvs_flash_init()
2. Drivers 初始化 bsp_init()：I2C / SPI / LEDC / PCA9557 / ST7789 / FT6336 / BOOT 键 / QMI8658
3. Peripherals 初始化 peripherals_init_all()：IO / Audio / LCD（含 LVGL display）/ Touch / IMU / Storage / Button
4. Services 启动：EventBus / Settings / Storage / Time / Audio / Net / Power / Notification
5. Framework 初始化：Theme / Asset / Window / AppMgr / UI / StatusBar / Notification / ControlCenter / Input
6. Apps 注册
7. 显示启动 logo 动画
8. 启动桌面
```

## 8. 内存预算

| 区域 | 大小 | 用途 |
|------|------|------|
| 内置 SRAM | 512 KB | FreeRTOS 任务栈、LVGL 控制块、LVGL 帧缓冲、关键 buffer |
| PSRAM | 8 MB | 字体、图片、视频缓冲、大块动态数据 |
| Flash App (factory + ota_0) | 各 4 MB | 用户应用、字库、图片素材 |
| Flash App (ota_1) | 4 MB | OTA 备份 |
| Flash FS (storage) | 3 MB | SPIFFS：配置、脚本、用户数据 |

**关键原则**：
- LVGL 帧缓冲放内置 DMA 内存（SPI 驱动无法直接 DMA PSRAM，见 `AGENTS.md` 4.2）
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
│   └── main.c                  # app_main()
├── apps/                       # Apps 层（含 src/app_register.c：App 注册表）
├── drivers/                    # Drivers 层
│   ├── CMakeLists.txt
│   ├── include/
│   └── src/
├── peripherals/                # Peripherals 层
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
├── managed_components/         # 组件管理器自动填充（按构建产物对待）
│   ├── lvgl__lvgl/
│   ├── espressif__esp_lvgl_port/
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
| Peripherals | 外设抽象层 |
| Drivers | 内核驱动 |
| Main (app_main) | 系统启动器 |

## 11. 关键技术决策

- **5 层架构**：每层职责独立，互不逆向调用
- **服务总线**：跨层通信统一通过 svc_event_bus
- **静态库组织**：每层编译为静态库，由 main 链接
- **LVGL 封装**：所有 UI 控件封装在 Framework 层
- **应用隔离**：每个 App 独立 NVS 命名空间 + 独立目录