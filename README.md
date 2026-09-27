# szpi-esp32s3

运行在立创·实战派 ESP32-S3 开发板上的嵌入式操作系统固件（SZPI-OS），基于 ESP-IDF v6.1，纯 C + Lua。

核心能力（界面、音频、摄像头、硬件 IO、文件、网络、蓝牙、系统信息）通过 Lua 脚本暴露给用户；固件本身只负责驱动、服务与框架。同一份固件里既有 22 个内置 App，也能随时跑用户放进 TF 卡的脚本。

## 特性

- **脚本驱动**：内置 Lua 5.5 运行时；脚本可以建界面、放音频、拍照、读传感器、联网、收发 BLE，还能给设备开局域网网页
- **完整界面**：LVGL 9 + esp_lvgl_port 2.x，深 / 浅两套主题，状态栏 + 桌面；中文内置 GB2312 字体，GBK 全集兜底
- **音频**：MP3 / WAV 播放（本地文件、HTTP(S) 链接、外部 PCM 流），WAV 录音，短提示音
- **摄像头**：GC0308 / GC2145 预览与拍照（24 位 BMP），不切 JPEG
- **网络**：Wi-Fi（STA / AP 配网 / SmartConfig）、HTTP(S)、MQTT、WebSocket，以及局域网 Web 管理页（系统状态 + 文件管理）
- **蓝牙**：BLE 4.2，广播 / 扫描 / GATT 从机 / HID 键鼠模拟 / GATT 客户端（中心角色）；配对提示全局弹出
- **存储**：TF 卡（FAT32）与内置 SPIFFS 双文件系统，支持文件管理、Web 上传下载、格式化
- **传感器**：QMI8658 六轴，判定姿态变化 / 摇晃 / 抬手（抬手可熄屏唤醒）
- **外扩接口**：GH1.25 引出的 GPIO10 / GPIO11，可复用为 GPIO、UART、PWM、CAN、ADC
- **无 OTA**：单体固件，分区表自定义

## 硬件平台

| 类别 | 器件 | 说明 |
|------|------|------|
| 主控 | ESP32-S3-WROOM-1-N16R8 | 双核 LX7 240 MHz，512 KB SRAM，8 MB PSRAM Octal 80 MHz，16 MB Flash |
| 显示屏 | ST7789 | 2.0 寸 IPS 320×240，SPI 80 MHz |
| 触摸 | FT6336 | 电容单点，I2C0 |
| 音频 DAC | ES8311 | I2C 配置 + I2S 输出 |
| 音频 ADC | ES7210 | 四通道（板载用 3 路），I2C 配置 + I2S 输入 |
| 功放 | NS4150B | 单声道 D 类，使能由 PCA9557 控制 |
| 摄像头 | GC0308 / GC2145 | DVP，按 PID 自动识别 |
| 姿态传感器 | QMI8658 | 六轴加速度 + 陀螺仪，I2C0 |
| IO 扩展 | PCA9557PW | LCD_CS、音频 PA_EN、摄像头 PWDN |
| 存储 | TF 卡 + 内置 Flash | SDMMC 1-bit（FAT32）/ SPIFFS |
| 外扩 | GH1.25 5P | GPIO10、GPIO11（GPIO / UART / PWM / CAN / ADC） |

触摸、IMU、IO 扩展、摄像头的 SCCB 共用 I2C0；摄像头由 PCA9557 的 PWDN 位控制上电。

## 架构

分 5 层，调用只能自上而下，不允许反向：

```
Apps / 脚本   22 个内置 App，以及 TF 卡上的 Lua 脚本
     ↓
Framework     AppMgr / Script / Window / Input / Theme / Asset / UI / StatusBar / Pairing
     ↓
Services      EventBus / Settings / Storage / Time / Audio / Net / BT / MQTT / WS / Power
              / IMU / IO / Camera / SysInfo / Web / Identity / Watchdog
     ↓
Peripherals   LCD / Touch / Audio / IMU / Storage / Camera / IO Expander / Button / Ext
     ↓
Drivers       BSP / I2C / PCA9557 / ST7789 / FT6336 / QMI8658 / ES8311 / ES7210 / Camera / Key / LEDC
     ↓
ESP-IDF + FreeRTOS + 第三方组件
```

跨层通信统一走事件总线（`svc_event_bus`）。界面操作遵守两条硬约束：非 LVGL 任务里碰 LVGL 必须加锁；App 不直接调 IDF / Peripherals / Drivers，缺接口就往 Services 加薄封装。

## 目录结构

```
szpi-esp32s3/
├── main/
│   ├── main.c              # app_main：启动序列
│   ├── drivers/            # Drivers 层（芯片级驱动）
│   ├── peripherals/        # Peripherals 层（外设抽象）
│   ├── services/           # Services 层（业务服务）
│   ├── framework/          # Framework 层（UI 框架 + 脚本运行时）
│   └── apps/               # Apps 层（22 个内置 App + 注册表）
├── tools/                  # 自检脚本、字体 / 图标生成
├── docs/                   # 需求 / 架构 / 详细设计
├── partitions.csv          # 分区表（factory 8 MB + storage 7 MB）
├── sdkconfig.defaults      # 默认配置（策略文件）
└── managed_components/     # 组件管理器自动填充，按构建产物对待
```

## 内置 App

桌面（Home）之间可以左右翻页，其余 22 个：

脚本管理、时钟、日历、天气、Wi-Fi、蓝牙 BLE、显示、声音、文件管理、文本编辑、计算器、下载器、音乐、录音机、相机、图库、秒表、计时器、姿态仪、性能监控、系统日志、关于本机。

文件管理里点音频 / 图片 / 文本会带路径直接交给对应 App 打开（`Music?path=`、`Image?path=`、`Editor?path=`）。

## Lua 脚本

- 脚本不放在固定目录：脚本管理会递归扫 TF 卡与内置存储整盘，收录所有 `.lua`（扩展名不分大小写）
- 固件自带 6 个示例（每次启动释放到 `/sdcard/scripts`，同名覆盖）：`counter.lua`、`clock.lua`、`snake.lua`、`io.lua`、`gomoku.lua` 与「脚本接口参考.txt」
- 脚本可以在文件头部用注释声明要用的能力模块：`-- @perm io,file,net`；不写表示全部可用
- 绑定覆盖：`ui`、`sys`、`timer`、`input`、`event`、`io`、`file`、`audio`、`net`、`mqtt`、`ws`、`bt`、`camera`、`imu`、`settings`、`util`、`json`、`web`。系统级设置（亮度 / 主题 / 时区）不开放给脚本
- 脚本硬件访问一律经 `svc_io`，不直接碰驱动；同一时刻只运行一个前台脚本，离开脚本页即停止

## 构建与烧录

需要 ESP-IDF v6.1（本项目在 Windows + PowerShell 下开发）。首次或切换目标时先 `set-target`，之后编译烧录：

```powershell
# 1. 进入 IDF 环境
C:\esp\v6.1\esp-idf\export.ps1

# 2. 仅首次或切换目标时
idf.py set-target esp32s3

# 3. 编译
idf.py build

# 4. 烧录并监视（端口按实际修改，默认 COM4）
idf.py -p COM4 flash monitor
```

改配置时先改 `sdkconfig.defaults`，再跑 `idf.py menuconfig` 让 `sdkconfig` 刷新；`sdkconfig` 与 `dependencies.lock` 都是自动生成的，不要手改。新增或删除源文件后要先 `idf.py reconfigure` 再 `idf.py build`（目录是配置期展开的）。用 `idf.py size-components` 查看 flash 占用。

## 自检与工具

改完代码或文案后跑一遍，都不依赖硬件、也不需要构建：

```powershell
python tools/check_cn_text.py           # 界面文案的非 ASCII 字符能否被字体覆盖，字体 cmap 是否含端点
python tools/check_ui_colors.py         # 创建了标签却没显式设色的地方
python tools/check_api_includes.py      # 调用的函数 / 宏其声明头是否可见
python tools/check_decl_order.py        # 文件内 static 定义晚于使用
python tools/check_lvgl_api.py          # 用到的 LVGL 标识符是否存在；列出带 %s 的 snprintf 供确认截断
python tools/check_comments.py          # 注释里的 Markdown 格式 / 引用块 / emoji / 外部文档引用
python tools/check_terms.py             # 术语写法（I2C / Wi-Fi / TF 卡 / SPIFFS）
python tools/check_deprecated_lvgl.py   # 是否用到 LVGL 已废弃的 API 别名
python tools/check_printf.py            # 是否直接用了 printf / puts（应统一走 ESP_LOGx）
python tools/check_tag.py               # 每个 .c 都有 TAG 且前缀与所在层一致
python tools/check_app_callbacks.py     # App 描述符的回调搭配（on_pause 要有 on_start / on_resume）
python tools/check_app_registry.py      # App 描述符定义与 app_common.c 注册一一对应
```

生成资源（改完脚本重新生成，不要手改生成出来的 C 文件）：

```powershell
python tools/gen_fw_fonts.py    # 中文字体（GB2312 两套 + GBK 回退）
python tools/gen_fw_icons.py    # 界面 / 状态栏 / 桌面三组图标
```

## 文档

- `docs/01-requirements/`：原始需求、硬件规格、产品需求（PRD）
- `docs/02-architecture/`：架构总览、分层设计、模块依赖
- `docs/03-design/`：外设层、服务层、UI 系统、应用与脚本框架、数据流

## 许可

MIT License，Copyright (c) 2026 Nie Mingzhao。详见 `LICENSE`。
