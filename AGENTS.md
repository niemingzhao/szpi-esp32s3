# AGENTS.md — szpi-esp32s3 (SZPI-OS)

## 一、语言与开发约定

- **语言约定**：本项目所有文档（`README.md`、`AGENTS.md`、`docs/`、代码注释）默认使用简体中文，与 `docs/` 现有内容保持一致。
- **文本与术语约定**：更新任一层文档或代码注释时，逐篇核对以下写法（最终以 `00-original-requirement.md`、`01-hardware-spec.md` 为准）：
  - `I2C`（不写 `I²C`）、`I2S`、`SPI`
  - `Wi-Fi`（不写 `WiFi`）
  - `SPIFFS`（不写 `LittleFS`）
  - 叙述中用 `TF 卡`（不写 `SD 卡`）
  - 中文叙述用 `App`（`Apps 层`、`apps/` 等英文标识除外）
  - 数字与单位之间加空格：`16 kHz`、`100 ms`、`30 FPS`
  - 不使用 emoji，不使用 `>` 引用块
  - 组件 / 头文件名以 ESP-IDF 实际为准（如 `bt`、`mqtt`、`vfs`、`json`，不写 `esp_bt`/`esp_mqtt`/`esp_vfs`/`cJSON`）
  - 涉及硬件参数时与 `01-hardware-spec.md` 核对（触摸为单点等）
- **开发约定**：AI 助手**只负责编写代码、编写文档、操作文件**。以下操作**严禁 AI 自行执行**，必须由人类开发者完成：
  - 构建项目（`idf.py build` / `idf.py set-target` 等）
  - 烧录固件到设备（`idf.py flash` / `esptool.py` 等）
  - 监视串口输出（`idf.py monitor` / 串口工具等）
  - 任何会与真实硬件交互的动作
- AI 可以生成这些命令供人类复制执行，但不要在工具调用里发起。

---

## 项目一句话定位

运行在立创·实战派 ESP32-S3 开发板（ESP32-S3-WROOM-1-N16R8）上的嵌入式固件 SZPI-OS。ESP-IDF **v5.4.4**，目标 `esp32s3`，xtensa-gcc。单体固件，纯 C；无 CI、无主机测试。

---

## 二、仓库入口（先看这里）

```
szpi-esp32s3/
├── CMakeLists.txt          # 顶层，include IDF project.cmake
├── partitions.csv          # 自定义分区表（在用）
├── sdkconfig               # 自动生成，禁止手改
├── sdkconfig.defaults      # 策略文件，可改
├── main/
│   ├── main.c              # app_main()，v0.4：Drivers + Peripherals + Services + Framework，启动 Home
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── drivers/                # 芯片级驱动（已完成）
├── peripherals/            # 外设抽象层（已完成）
├── services/               # 业务服务层（基础服务已实现，audio / net 为骨架）
├── framework/              # UI 框架层（scope A：theme / asset / window / app_mgr / statusbar / input）
├── apps/                   # 应用层（scope A：app_home 桌面 + app_clock 示例）
├── managed_components/     # 组件管理器自动填充，按构建产物对待
├── docs/                   # 设计文档（需求 / 架构 / 详细设计）
│   ├── 01-requirements/    # 原始需求、硬件规格、PRD
│   ├── 02-architecture/    # 架构总览、分层、模块依赖
│   └── 03-design/          # Peripherals / Services / UI / App 框架 / 数据流
└── build/                  # 构建产物（在 .gitignore）
```

**关键文件说明**：

- `main/main.c` —— v0.4 启动入口：NVS → `bsp_init()`（Drivers 层：I2C0 GPIO1/2 100 kHz → LEDC 背光 GPIO42 → PCA9557 @ 0x19 → ST7789 屏 SPI3_HOST 40/41/39 80 MHz 模式 2 → FT6336 单点触摸 @ 0x38 → BOOT 键 GPIO0 → QMI8658 @ 0x6A）→ `peripherals_init_all()`（IO / Audio / LCD+LVGL / Touch / IMU / Storage / Button）→ `services_init()`（EventBus / Settings / Storage / Time / Audio / Net / Power / Notification）→ `fw_init()`（Framework 层）→ `app_register_all()` → `fw_app_mgr_launch("Home")` 显示桌面 → 挂载内置 SPIFFS。
- `main/idf_component.yml` —— 声明依赖：`idf >=5.4.0`、`lvgl/lvgl ~8.3.0`、`espressif/esp_lvgl_port ~1.4.0`、`espressif/esp_lcd_touch_ft5x06 ~1.0.7`。
- `dependencies.lock` —— 精确锁定版本，**禁止手改**。升级时改 `main/idf_component.yml` 或 `sdkconfig.defaults`，让构建工具重新生成。
- `partitions.csv` —— 自定义分区表，已在用（见文件头）。**不要切换到内置分区方案**，否则必须同步修改 factory/ota 布局和文档。

---

## 三、5 层架构（五层均已建立，Framework / Apps 为 scope A）

调用方向（**禁止反向调用**）：

```
Apps → Framework → Services → Peripherals → Drivers → ESP-IDF/FreeRTOS
```

| 目录 | 模块 | 说明 |
|------|------|------|
| `drivers/` | `pca9557`、`st7789`、`ft6336`、`qmi8658`、BOOT 按键、LEDC、BSP | 已实现（`es8311`、`es7210`、`gc0308`、SDMMC 独立驱动规划中） |
| `peripherals/` | `periph_lcd_*`、`periph_touch_*`、`periph_audio_*`、`periph_imu_*`、`periph_storage_*`、`periph_io_exp_*`、`periph_button_*` | 部分实现：LCD/Touch/Button/IMU/Storage/IO 已实现；Audio 占位；Camera 未包含 |
| `services/` | `svc_event_bus`、`svc_settings`、`svc_time`、`svc_audio`、`svc_net`、`svc_storage`、`svc_notification`、`svc_power` | 骨架 + 基础服务：event_bus / settings / storage / time / power / notification 已实现；audio / net 为骨架 |
| `framework/` | `fw_app_mgr`、`fw_window`、`fw_input`、`fw_theme`、`fw_asset`、`fw_statusbar` | scope A 已实现；`fw_control_center`、`fw_notification` 规划中 |
| `apps/` | `app_home`、`app_clock`（scope A） | 已实现这 2 个；`app_music`、`app_settings` 等其余 15 个规划中 |

五层目录均已建立：`drivers/`、`peripherals/`、`services/`、`framework/`、`apps/`。新增模块时遵守 `docs/02-architecture/01-layer-design.md`：

- **命名**：`drv_<chip>_*`、`bsp_*`、`periph_<dev>_*`、`svc_<svc>_*`、`fw_<mod>_*`、`app_<name>_*`。
- **返回值**：所有公开 API 返回 `esp_err_t`。
- **日志**：禁用 `printf`，统一用 `ESP_LOGI/W/E`，TAG 带层前缀（`"drv.st7789"`、`"periph.lcd"`、`"svc.audio"`、`"fw.window"`、`"app.clock"`）。
- **单向调用**：Peripherals 不能调 Services，Apps 不能调 Peripherals/Drivers，Services 不能调 Framework/Apps。
- **例外**：Peripherals / Services / Framework 中可以使用 `lvgl_port_lock/unlock`；`fw_input` 可直接注册 `periph_button` 回调（按键事件尚未接入事件总线）。
- **任务模型**：每个 Service 通常独占一个 FreeRTOS 任务；同步 API 只用于简单 setter。

---

## 四、AI 易踩的坑（按出错的代价排序）

### 4.1 LVGL 调用必须加锁

在非 LVGL 任务里（触摸扫描任务、App 回调、Services、Framework 初始化）调用任何 LVGL API，**必须**用 `lvgl_port_lock(0)` / `lvgl_port_unlock()` 包起来。`lvgl_port` 用的是递归互斥锁，因此在 LVGL 事件回调内再次加锁是安全的。

全局浮层挂在 `lv_layer_top()` 上时，必须先 `lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE)`：`lv_obj` 默认带 `LV_OBJ_FLAG_CLICKABLE`，否则浮层会吞掉全屏触摸（LVGL 命中顺序为 layer_sys → layer_top → 当前屏）。

### 4.2 LVGL framebuffer 必须放 PSRAM

`lvgl_port_display_cfg_t.flags.buff_spiram = true`（骨架已设）。`CONFIG_LV_MEM_CUSTOM=y`、`CONFIG_LV_USE_PNG=y`、`CONFIG_LV_USE_GIF=y`（目标配置见 `docs/01-requirements/01-hardware-spec.md`）：PNG/GIF 解码器会明显增大固件体积，调整前先评估 flash 预算。

### 4.3 `CONFIG_LV_COLOR_16_SWAP=y` 与 ST7789 RGB element order 配套

除非换面板，否则保持原样。

### 4.4 硬件细节：PCA9557 控制的不只是 IO

| 信号 | 实际位置 |
|------|----------|
| LCD CS | **PCA9557.BIT0**（不是 GPIO）—— `esp_lcd_panel_init` 前要 `drv_pca9557_set_pin(DRV_PCA9557_LCD_CS, 0)` 拉低 |
| 音频 PA_EN | PCA9557.BIT1 |
| 摄像头 PWDN | PCA9557.BIT2 |
| LCD RST | **NC**（靠 `esp_lcd_panel_reset()` 软件复位） |
| 触摸 INT | **NC**（轮询模式） |
| 触摸 RST | **NC** |

完整引脚/I2C 表以 `docs/01-requirements/01-hardware-spec.md` 为准。

- 触摸为单点（FT6336），不支持多指手势。
- GPIO33~37 被八线 PSRAM 占用，不可用；GPIO26~32 为模组内 Flash/PSRAM，未引出。
- UART0（GPIO43/44）接 CH340K，用于下载与串口调试。

### 4.5 配置文件的修改边界

- `sdkconfig` 自动生成，**禁止手改**。
- `sdkconfig.defaults` 是策略文件，可以改。
- `dependencies.lock` 禁止手改。
- `partitions.csv` 自定义表，不要切到内置方案。

### 4.6 蓝牙协议栈是 Bluedroid，不是 NimBLE

`CONFIG_BT_BLUEDROID_ENABLED=y`，`sdkconfig` 里 NimBLE 显式关闭。引入 NimBLE 专有示例前必须重新评估。

### 4.7 OTA 必须 HTTPS

`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` 关闭，OTA 解密回调也关闭。

### 4.8 FATFS 三项必须同时保留

`storage` 分区上 `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_CODEPAGE_936=y`、`CONFIG_FATFS_API_ENCODING_UTF_8=y` 三项必须一起保留，否则 TF 卡上的非 ASCII 长文件名会乱码。

---

## 五、存储与分区预算（不要超）

| 资源 | 容量 | 用途 |
|------|------|------|
| Flash | 16 MB | 全部固件 + 资源 |
| PSRAM | 8 MB Octal @ 80 MHz | LVGL framebuffer、大 buffer |

分区布局（`partitions.csv`）：

| 分区 | 偏移 | 大小 | 类型 |
|------|------|------|------|
| `nvs` | 0x9000 | 24 KB | NVS |
| `phy_init` | 0xf000 | 4 KB | PHY 校准 |
| `factory` | — | 4 MB | App |
| `ota_0` | — | 4 MB | OTA |
| `ota_1` | — | 4 MB | OTA 备份 |
| `storage` | — | 3 MB | SPIFFS |

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048`：小分配走内部 SRAM，≥2 KB 走 PSRAM。
- 添加大资源前先让人类执行 `idf.py size-components` 检查。
- 仓库位于 `D:\workspace\szpi-esp32s3`（Windows 路径，无空格；`.vscode` 配置写死）。

---

## 六、构建 / 烧录工作流（命令由人类执行）

**AI 不要自行运行以下任何命令**，仅供文档记录与人类复制：

```powershell
# 1. 进入 IDF 环境（PowerShell）
C:\esp\v5.4.4\esp-idf\export.ps1

# 2. 仅首次或切换目标时
idf.py set-target esp32s3

# 3. 编译
idf.py build

# 4. 烧录（端口按实际修改）
idf.py -p COM4 flash

# 5. 串口监视（115200 8N1，Ctrl+] 退出）
idf.py -p COM4 monitor

# 6. 编译并烧录并监视
idf.py -p COM4 flash monitor

# 7. 改配置（先改 sdkconfig.defaults，再跑 menuconfig 让 sdkconfig 刷新）
idf.py menuconfig

# 8. 检查 flash/堆预算
idf.py size
idf.py size-components
idf.py size-files
```

- 默认烧录串口 `COM4`（`.vscode/settings.json` 的 `idf.port`）。
- 默认监视波特率 `CONFIG_ESPTOOLPY_MONITOR_BAUD=115200`。
- 调试：`.vscode/launch.json` 是 `gdbtarget` attach 配置，OpenOCD + `board/esp32s3-bridge.cfg`（来自 `idf.openOcdConfigs`）。
- LSP：clangd 读取 `build/compile_commands.json`。让人类先跑一次 `idf.py build`，`--compile-commands-dir` 已在 `.vscode/settings.json` 固定为 `d:\workspace\szpi-esp32s3\build`。

---

## 七、启动序列（v0.4 已实现到启动桌面）

```
1. nvs_flash_init()
2. bsp_init()                 → I2C, SPI, LEDC, PCA9557, LCD, Touch, Key, IMU
3. peripherals_init_all()     → IO, Audio, LCD(+LVGL display), Touch, IMU, Storage, Button
                                 └─ periph_lcd_init() 内 lvgl_port_init() + lvgl_port_add_disp()
                                    periph_touch_init() 注册 LVGL input device
4. services_init()            → EventBus, Settings, Storage, Time, Audio, Net, Power, Noti
5. fw_init()                  → Theme, Asset, Window, AppMgr, StatusBar, Input（并创建全局浮层）
6. app_register_all()         → 注册所有内置 App
7. fw_app_mgr_launch("Home")  → 显示桌面
8. periph_storage_mount(内置) → 首屏之后挂载内置 SPIFFS（首次自动格式化）

（未实现：fw_boot_animation、fw_control_center、fw_notification）
```

时间预算（目标到首屏 < 2.5 s）见 `docs/03-design/04-data-flow.md`。

---

## 八、典型数据流

### 触摸事件
drv_ft6336 → periph_touch 扫描任务（缓存 + 手势识别）→ LVGL indev read_cb 读缓存 → LVGL input device 派发 → App 处理

### 音频播放
App 调用 `svc_audio_play(path)` → svc_audio 任务 → svc_storage 读文件 → helix MP3 解码 → I2S DMA → ES8311 → NS4150B 功放 → 喇叭

### 配置持久化
App 调用 `svc_settings_set(key, value)` → NVS write → 触发 `SVC_EVENT_*_CHANGED` → 订阅该事件的 App 自行刷新 UI（App 在 `on_start` 订阅、`on_pause` 退订）

详细见 `docs/03-design/04-data-flow.md`。

---

## 九、真相来源优先级（与文档冲突时）

1. `main/main.c` + `sdkconfig` + `partitions.csv` + `dependencies.lock`（真正会参与构建的）
2. `docs/02-architecture/*.md`（目标架构）
3. `docs/01-requirements/*.md`（意图）
4. `docs/03-design/*.md`（详细设计）

(1) 与 (2) 冲突时，以 (1) 为准并更新 (2)。

---

## 十、本仓库**没有**的东西（不要去找）

- 没有 CI 工作流（无 `.github/`、无 `.gitlab-ci.yml`）。
- 没有主机端单元测试，没有 Unity / pytest-embedded 框架，没有代码覆盖率配置。
- 没有 pre-commit 钩子，没有 `clang-format`、没有 linter 配置。
- 没有 Docker / devcontainer。
- 没有 `CLAUDE.md` / `.cursor/rules`（`AGENTS.md` 是唯一的 agent 指引）。

如需新增其中任何一项，**先与人类确认**再提议。