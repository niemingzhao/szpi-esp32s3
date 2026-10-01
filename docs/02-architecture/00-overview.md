# 架构总览

## 1. 设计目标

| 目标 | 含义 |
|------|------|
| 流畅 UI | 触摸、状态栏、脚本界面响应迅速 |
| 脚本驱动 | 用户用 Lua 脚本扩展系统能力，无需重新编译固件 |
| 可学习 | 五层职责清晰，源码目录集中 |
| 可扩展 | 新增 App 简单，Peripherals 可替换 |
| 可裁剪 | 各层按需编译，脚本按需运行 |

## 2. 分层架构图

```
┌──────────────────────────────────────────────────────────┐
│ Apps 应用层   脚本管理器 / 时钟 / 日历 / 天气 / Wi-Fi / …  │
└───────────────────────┬──────────────────────────────────┘
                        │ app_if.h
┌───────────────────────▼──────────────────────────────────┐
│ Framework 框架层   AppMgr / Script / Window / Input /     │
│                    Theme / Asset / UI / StatusBar         │
└───────────────────────┬──────────────────────────────────┘
                        │ svc_if.h
┌───────────────────────▼──────────────────────────────────┐
│ Services 服务层    EventBus / Settings / Storage / Time / │
│    Audio / Net / BT / MQTT / WS / Power / IMU / IO / …    │
└───────────────────────┬──────────────────────────────────┘
                        │ periph_if.h
┌───────────────────────▼──────────────────────────────────┐
│ Peripherals 外设层  LCD / Touch / Audio / IMU / Storage / │
│                     Camera / IO Exp / Button / Ext        │
└───────────────────────┬──────────────────────────────────┘
                        │ drv_xxx.h
┌───────────────────────▼──────────────────────────────────┐
│ Drivers 驱动层  BSP / I2C / PCA9557 / ST7789 / FT6336 /   │
│                 QMI8658 / ES8311 / ES7210 / Camera / Key │
└───────────────────────┬──────────────────────────────────┘
                        │
┌───────────────────────▼──────────────────────────────────┐
│ ESP-IDF + FreeRTOS + 第三方组件                            │
└──────────────────────────────────────────────────────────┘
```

## 3. 各层职责定义

### 3.1 Drivers 层

**职责**：与 ESP-IDF 驱动直接对接的薄封装。

**包含**：
- `bsp_init` - 板级初始化顺序管理器
- `i2c` - I2C0 主机总线（新版 i2c_master）+ 寄存器读写封装
- `pca9557` - IO 扩展芯片驱动
- `st7789` - LCD 面板驱动
- `ft6336` - 触摸芯片驱动
- `qmi8658` - IMU 驱动
- `es8311` - 音频 DAC 驱动
- `es7210` - 音频 ADC 驱动
- `camera` - DVP 摄像头驱动（兼容 GC0308 / GC2145）
- `key` - BOOT 按键驱动
- `ledc` - 背光 PWM 封装

**约束**：
- 不调用其他任何 SZPI-OS 层
- 只依赖 ESP-IDF / FreeRTOS / 第三方组件
- 不创建 FreeRTOS 任务（例外：`drv_key` 的去抖任务 `key_task`；其余驱动只注册 ISR）

### 3.2 Peripherals 层

**职责**：面向业务的外设抽象，隐藏具体芯片型号。

**包含**：
- `periph_lcd` - 显示屏
- `periph_touch` - 触摸
- `periph_audio` - 音频播放 / 录音
- `periph_imu` - 姿态
- `periph_storage` - TF 卡 + 内置 Flash
- `periph_camera` - 摄像头
- `periph_io_exp` - IO 扩展
- `periph_button` - BOOT 按键
- `periph_ext` - 外扩接口（GPIO / PWM / I2C / UART / ADC）

**约束**：
- 只调用 Drivers 层 + ESP-IDF
- 不依赖 Services / Framework / Apps
- 提供面向业务的统一 API（如音量用 0-100）

### 3.3 Services 层

**职责**：跨外设的业务服务，处理协议、状态与并发。

**包含**：
- `svc_event_bus` - 事件总线（贯穿所有层）
- `svc_settings` - 配置持久化（NVS，含 blob）
- `svc_storage` - 文件系统与路径管理
- `svc_time` - 时间与时区
- `svc_audio` - 音频播放、录音、提示音
- `svc_net` - Wi-Fi 配网与管理、HTTP(S) 客户端
- `svc_bt` - 蓝牙 BLE（广播、扫描、GATT 从机、HID 模拟）
- `svc_mqtt` - MQTT 客户端
- `svc_ws` - WebSocket 客户端
- `svc_power` - 背光、熄屏、唤醒、关机 / 重启
- `svc_imu` - IMU 姿态与运动
- `svc_io` - 外扩 GPIO / PWM / I2C / UART / ADC
- `svc_camera` - 摄像头（把 esp32-camera 挡在服务层内）
- `svc_sysinfo` - 系统信息与崩溃记录
- `svc_watchdog` - Task Watchdog

**约束**：
- 调用 Peripherals 层
- 不调用 Framework / Apps
- 每个服务独占一个 FreeRTOS 任务（必要时）

### 3.4 Framework 层

**职责**：UI 框架与脚本运行时，提供原生 App 与脚本两种应用模型。

**包含**：
- `fw_app_mgr` - 原生 App：注册、生命周期、返回栈
- `fw_script` - 脚本：Lua 运行时、脚本管理、脚本绑定
- `fw_window` - 窗口（屏）切换
- `fw_input` - 输入路由（触摸 / 按键 / 姿态）
- `fw_theme` - 主题
- `fw_asset` - 字体与图标资源
- `fw_ui` - 通用 UI 组件（进度条 / 对话框 / Toast / 列表 / 网格）
- `fw_statusbar` - 状态栏
- `fw_boot_animation` - 开机画面与提示音

**约束**：
- 调用 Services 层
- 强依赖 LVGL
- 不直接调用 Peripherals（必须经 Services）

### 3.5 Apps 层

**职责**：内置应用，每个 App 一个子目录，编译进同一固件。

**内置应用（22 个）**：
- `app_scripts` - 脚本管理器
- `app_clock` - 时钟
- `app_calendar` - 日历
- `app_weather` - 天气
- `app_wifi` - Wi-Fi
- `app_bt` - 蓝牙 BLE
- `app_display` - 显示
- `app_sound` - 声音
- `app_file` - 文件管理器
- `app_editor` - 文本编辑器
- `app_calc` - 计算器
- `app_download` - 下载器
- `app_music` - 音乐播放器
- `app_recorder` - 录音机
- `app_camera` - 相机
- `app_image` - 图片查看器
- `app_stopwatch` - 秒表
- `app_timer` - 计时器
- `app_imu` - 姿态传感器
- `app_perf` - 性能监控
- `app_log` - 系统日志
- `app_about` - 关于本机

**约束**：
- 调用 Framework + Services 层
- 不直接调用 Peripherals
- 通过 `fw_app_mgr_register` 注册

### 3.6 脚本子系统

系统在原生 App 之外提供第二套应用模型：脚本用 Lua 5.5 编写，存放在 TF 卡脚本目录，由脚本管理器选择运行。运行时、管理与绑定都在 `fw_script`。

**运行时**：Lua 5.5，提供加载 / 执行 / 停止接口与错误处理。

**脚本管理**：扫描脚本目录、读取元信息、释放内置示例脚本、下载网络脚本。

**能力绑定**：
- 界面 / 输入 / 通知 - 调用 Framework 自身（`fw_ui` / `fw_input`）
- 音频 / 摄像头 / 文件 / 网络 / BLE / 时间 / 系统信息 / 事件 - 调用 Services
- GPIO / PWM / I2C / UART / ADC - 调用 `svc_io`

**权限与沙箱**：脚本头部用 `-- @perm io,file,net` 声明要用的能力模块；不写声明表示全部可用，写了声明就只注册列出的模块。

**生命周期**：同一时刻只运行一个前台脚本；脚本退出或异常时释放资源，不影响系统。

脚本与原生 App 共用事件总线、配置系统与存储。

### 3.7 main/ 入口

**职责**：系统入口与启动序列。

**包含**：
- `main.c` - `app_main()`
- `main/apps/src/app_register.c` - 所有内置 App 的注册表

## 4. 关键技术选型

| 技术 | 选择 | 用途 |
|------|------|------|
| 操作系统 | FreeRTOS | ESP-IDF 内置 |
| GUI | LVGL 9 | 嵌入式 GUI |
| LVGL 适配 | esp_lvgl_port 2.x | 显示与线程安全 |
| 脚本 | Lua 5.5（espressif/lua） | 用户脚本运行时 |
| 文件系统 | FAT32（TF 卡）+ SPIFFS（内置 Flash） | 双文件系统 |
| 持久化 | NVS | 系统配置、Wi-Fi 凭据 |
| 音频解码 | helix MP3 | MP3 解码 |
| 蓝牙 | Bluedroid | 仅 BLE |
| 摄像头 | esp32-camera | GC0308 / GC2145 |
| HTTP | esp_http_client | HTTP / HTTPS 客户端 |
| MQTT | mqtt 组件 | MQTT 客户端 |
| WebSocket | esp_websocket_client | WebSocket 客户端 |

## 5. 数据流

### 5.1 触摸事件流

```
FT6336 轮询（单点）
  → periph_touch 扫描任务：缓存坐标 + 点击 / 长按识别
    → LVGL input device（read_cb 读取缓存）
      → 当前界面控件回调（原生 App 或脚本）
```

### 5.2 音频播放流

```
App / 脚本 调用 svc_audio_play(path)
  → svc_audio 任务接收请求
    → svc_storage 读取文件
    → helix MP3 解码
    → I2S0 DMA buffer
      → ES8311 → NS4150B 功放 → 喇叭
```

### 5.3 脚本运行流

```
脚本管理器（app_scripts）选择脚本
  → fw_script 加载 Lua 脚本
    → 脚本调用绑定：
        界面 / 输入 / 通知 → fw_ui / fw_input
        音频 / 摄像头 / 文件 / 网络 / BLE / 系统 → Services
        GPIO / PWM / I2C / UART / ADC → svc_io
    → 停止或异常退出 → 释放资源，回到脚本管理器
```

### 5.4 配置持久化流

```
App / 脚本 调用 svc_settings_set(key, value)
  → NVS write
  → 发布 SVC_EVENT_*_CHANGED
    → 订阅者收到事件并刷新
```

## 6. 任务规划

| 任务名 | 优先级 | 核心 | 职责 |
|--------|--------|------|------|
| `main_task` | 1 | 0 | 启动序列 |
| `lvgl_task` | 5 | - | LVGL tick + render |
| `touch_scan_task` | 4 | 0 | 触摸扫描、点击 / 长按识别 |
| `key_task` | 4 | 0 | BOOT 按键 |
| `imu_task` | 3 | 0 | 周期性读取 IMU |
| `svc_audio` | 6 | 1 | 音频解码 / 录音 + I2S 读写 |
| `ntp_sync_task` | 2 | 0 | SNTP 周期同步 |
| `power_task` | 2 | 0 | 背光超时、熄屏 |
| `sd_monitor_task` | 3 | 0 | TF 卡热插拔监测 |
| `script_task` | 4 | 0 | 脚本执行 |
| `event_bus_task` | 4 | 0 | 事件分发 |

任务间通信：LVGL 操作经 `lvgl_port_lock/unlock`；跨任务用 FreeRTOS 队列或事件组；全局状态用互斥锁保护。Wi-Fi / IP 事件由 ESP-IDF 的 `esp_event` 默认事件循环任务处理，`svc_net` 的 HTTP 请求按需创建临时任务 `svc.http`（优先级 5），`app_wifi` 扫描时按需创建临时任务 `wifi_scan`（优先级 4）。

## 7. 启动序列

```
1. nvs_flash_init()
2. bsp_init()                // I2C / LEDC / PCA9557 / ST7789 / FT6336 / BOOT 键 / QMI8658
3. peripherals_init_all()    // IO / Audio / LCD(+LVGL) / Touch / IMU / Storage / Button / Ext
4. services_init()           // Watchdog → EventBus → Settings → Storage → BT → Time →
                             //   Audio → Net → Power → IMU → IO → Camera → SysInfo
5. fw_init()                 // Theme / Asset / Window / AppMgr / Script / UI / StatusBar / Input
6. app_register_all()        // 注册所有内置 App
7. fw_boot_animation()       // 开机画面 + 提示音
8. fw_app_mgr_launch("Home") // 进入桌面
9. periph_storage_mount(内置 Flash)  // 首屏之后挂载
10. svc_watchdog_arm()       // 打开超时自动重启
```

## 8. 内存与分区

### 8.1 内存

| 区域 | 大小 | 用途 |
|------|------|------|
| 内置 SRAM | 512 KB | FreeRTOS 任务栈、LVGL 控制块、绘制缓冲、关键 buffer |
| PSRAM | 8 MB | 字体 / 图片 / 音频 / 摄像头缓冲、大块动态数据 |

**关键原则**：
- LVGL 绘制缓冲放内置 DMA 内存（SPI 驱动无法直接 DMA PSRAM，见 `AGENTS.md` 4.2）
- 非关键 buffer 优先 PSRAM
- 关键实时路径（音频解码、触摸）放内置 SRAM

### 8.2 分区表（16 MB Flash）

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  24k
phy_init, data, phy,     0xf000,  4k
factory,  app,  factory, ,        8M
storage,  data, spiffs,  ,        7M
```

合计：bootloader（32KB @ 0x0000）+ partition table（12KB @ 0x8000）+ 4 个分区（24K + 4K + 8M + 7M ≈ 15M）≈ 15.07MB，16MB Flash 余量约 1MB

不做 OTA，所以没有 ota_0 / ota_1；`factory` 留足余量，其余给 SPIFFS。实际文件为仓库根目录的 `partitions.csv`。

## 9. 关键配置项（sdkconfig.defaults）

LVGL 用 9.x，脚本运行时用 `espressif/lua` 组件（Lua 5.5）。

```ini
# 目标与分区
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y

# CPU 与缓存
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y

# FreeRTOS 运行时统计（性能监控）
CONFIG_FREERTOS_USE_TRACE_FACILITY=y
CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y

# PSRAM（内部 RAM 紧张：小分配优先 PSRAM，并预留内部 DMA 区）
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=16384
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y

# Wi-Fi（静态收发缓冲，省内部 RAM）
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8

# FATFS（TF 卡非 ASCII 长文件名）
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_CODEPAGE_936=y
CONFIG_FATFS_API_ENCODING_UTF_8=y

# SPIFFS（内置 Flash 文件系统）
CONFIG_SPIFFS_OBJ_NAME_LEN=128

# LVGL 9 配置（lv_conf 宏）
#   LV_COLOR_DEPTH        16
#   LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB（走 IDF 堆，受 PSRAM 分配策略约束）
#   LV_FONT_MONTSERRAT_20 / 24 / 32   1
#   LV_FONT_FMT_TXT_LARGE 1
#   LV_USE_LODEPNG / LV_USE_GIF / LV_USE_BMP / LV_USE_TJPGD   1
# RGB565 字节交换在显示 flush 回调里处理（LVGL 9 已移除 LV_COLOR_16_SWAP）

# 蓝牙 BLE（仅 4.2；控制器环境池保持默认，勿下调）
CONFIG_BT_ENABLED=y
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set
CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y
CONFIG_BT_ACL_CONNECTIONS=2
CONFIG_BT_CTRL_BLE_MAX_ACT=6

# 摄像头（GC0308 / GC2145；SCCB 走新版 i2c_master + I2C1）
CONFIG_GC0308_SUPPORT=y
CONFIG_GC2145_SUPPORT=y
CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW=y
CONFIG_SCCB_HARDWARE_I2C_PORT1=y
```

## 10. 目录结构

```
szpi-esp32s3/
├── CMakeLists.txt          # 顶层，EXTRA_COMPONENT_DIRS 指向 main 下各层
├── partitions.csv          # 分区表
├── sdkconfig.defaults      # 默认配置
├── main/
│   ├── CMakeLists.txt
│   ├── main.c              # app_main()
│   ├── drivers/            # Drivers 层
│   ├── peripherals/        # Peripherals 层
│   ├── services/           # Services 层
│   ├── framework/          # Framework 层
│   └── apps/               # Apps 层（含 src/app_register.c）
├── managed_components/     # 组件管理器自动填充（按构建产物对待）
├── tools/                  # 自检脚本与字体生成
└── docs/
    ├── 01-requirements/
    ├── 02-architecture/
    └── 03-design/
```

## 11. 架构类比

| SZPI-OS | 对应概念 |
|---------|----------|
| Apps | 内置应用 |
| 脚本 | 用户可编程的应用 |
| Framework | 窗口管理 + 控件系统 + 脚本运行时 |
| Services | 系统服务（音频 / 网络 / 存储 / IO） |
| Peripherals | 外设抽象层 |
| Drivers | 内核驱动 |
| main | 系统启动器 |

## 12. 关键技术决策

- **5 层架构**：每层职责独立，互不逆向调用
- **脚本驱动**：Lua 运行时与绑定集中在 `fw_script`，脚本与原生 App 并存
- **服务总线**：跨层通信统一通过 `svc_event_bus`
- **静态库组织**：每层编译为静态库，由 main 链接
- **LVGL 封装**：所有 UI 控件封装在 Framework 层
- **资源隔离**：每个 App 独立 NVS 命名空间
