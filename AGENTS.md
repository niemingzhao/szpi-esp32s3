# AGENTS.md — szpi-esp32s3 (SZPI-OS)

## 语言与开发约定

- **语言约定**：本项目所有文档（`README.md`、`AGENTS.md`、`docs/`、代码注释）默认使用简体中文，与 `docs/` 现有内容保持一致。
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
│   ├── main.c              # app_main()，当前只有 v0.1 骨架
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── managed_components/     # 组件管理器自动填充，按构建产物对待
├── docs/                   # 设计文档（需求 / 架构 / 详细设计）
│   ├── 01-requirements/    # 原始需求、硬件规格、PRD
│   ├── 02-architecture/    # 架构总览、分层、模块依赖
│   └── 03-design/          # HAL / Services / UI / App 框架 / 数据流
└── build/                  # 构建产物（在 .gitignore）
```

**关键文件说明**：

- `main/main.c` —— 仅有 `app_main()` 的 v0.1 骨架：NVS → I2C0（GPIO1/2，100 kHz）→ PCA9557 @ 0x19 → LEDC 背光（GPIO42）→ ST7789 屏（SPI3_HOST：MOSI GPIO40、CLK GPIO41、80 MHz、模式 2）→ FT6336 触摸 @ 0x38 → `esp_lvgl_port`（LVGL 8.3.0）→ 一个按钮 + 标签。
- `main/idf_component.yml` —— 声明依赖：`idf >=5.4.0`、`lvgl/lvgl ~8.3.0`、`espressif/esp_lvgl_port ~1.4.0`、`espressif/esp_lcd_touch_ft5x06 ~1.0.7`。
- `dependencies.lock` —— 精确锁定版本，**禁止手改**。升级时改 `main/idf_component.yml` 或 `sdkconfig.defaults`，让构建工具重新生成。
- `partitions.csv` —— 自定义分区表，已在用（见文件头）。**不要切换到内置分区方案**，否则必须同步修改 factory/ota 布局和文档。

---

## 三、计划的 5 层架构（`docs/` 中定义，**代码尚未实现**）

调用方向（**禁止反向调用**）：

```
Apps → Framework → Services → HAL → Drivers → ESP-IDF/FreeRTOS
```

| 目录 | 模块 | 说明 |
|------|------|------|
| `drivers/` | `pca9557`、`st7789`、`ft6336`、`qmi8658`、`es8311`、`es7210`、`gc0308`、SDMMC、BOOT 按键、LEDC | 芯片级薄封装 |
| `hal/` | `hal_lcd_*`、`hal_audio_*`、`hal_imu_*`、`hal_storage_*`、`hal_camera_*`、`hal_io_exp_*`、`hal_button_*` | 面向业务的 C API（如亮度 0-100） |
| `services/` | `svc_event_bus`、`svc_time`、`svc_audio`、`svc_net`、`svc_storage`、`svc_notification`、`svc_power` | 跨 HAL 业务逻辑 |
| `framework/` | `fw_app_mgr`、`fw_window`、`fw_input`、`fw_theme`、`fw_asset`、`fw_statusbar`、`fw_control_center`、`fw_notification` | LVGL 封装、App 编程模型 |
| `apps/` | `app_clock`、`app_music`、`app_settings` 等 17 个内置 app | 通过 `FW_APP_REGISTER(...)` 注册 |

这些目录当前**还不存在**。新建时遵守 `docs/02-architecture/01-layer-design.md`：

- **命名**：`drv_<chip>_*`、`bsp_*`、`hal_<dev>_*`、`svc_<svc>_*`、`fw_<mod>_*`、`app_<name>_*`。
- **返回值**：所有公开 API 返回 `esp_err_t`。
- **日志**：禁用 `printf`，统一用 `ESP_LOGI/W/E`，TAG 带层前缀（`"drv.st7789"`、`"hal.lcd"`、`"svc.audio"`、`"fw.window"`、`"app.clock"`）。
- **单向调用**：HAL 不能调 Services，Apps 不能调 HAL/Drivers，Services 不能调 Framework/Apps。
- **唯一例外**：Services 回调中可以使用 `lvgl_port_lock/unlock`。
- **任务模型**：每个 Service 通常独占一个 FreeRTOS 任务；同步 API 只用于简单 setter。

---

## 四、AI 易踩的坑（按出错的代价排序）

### 4.1 LVGL 调用必须加锁

在非 LVGL 任务里（触摸 ISR、app 回调、Services）调用任何 LVGL API，**必须**用 `lvgl_port_lock(0)` / `lvgl_port_unlock()` 包起来。骨架代码里的按钮回调和 `create_ui` 已经是范式，照抄。

### 4.2 LVGL framebuffer 必须放 PSRAM

`lvgl_port_display_cfg_t.flags.buff_spiram = true`（骨架已设）。`sdkconfig.defaults` 里 `CONFIG_LV_MEM_CUSTOM=y`、`CONFIG_LV_USE_PNG=n`、`CONFIG_LV_USE_GIF=n` 是有意的：PNG/GIF 解码器会让 flash 体积明显膨胀，改回前必须先评估预算。

### 4.3 `CONFIG_LV_COLOR_16_SWAP=y` 与 ST7789 RGB element order 配套

除非换面板，否则保持原样。

### 4.4 硬件细节：PCA9557 控制的不只是 IO

| 信号 | 实际位置 |
|------|----------|
| LCD CS | **PCA9557.BIT0**（不是 GPIO）—— `esp_lcd_panel_init` 前要 `pca9557_set_output(LCD_CS_GPIO, 0)` 拉低 |
| 音频 PA_EN | PCA9557.BIT1 |
| 摄像头 PWDN | PCA9557.BIT2 |
| LCD RST | **NC**（靠 `esp_lcd_panel_reset()` 软件复位） |
| 触摸 INT | **NC**（轮询模式） |
| 触摸 RST | **NC** |

完整引脚/I2C 表见 `docs/01-requirements/01-hardware-spec.md`（如与 `main.c` 注释冲突，以此为准）。

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

`storage` 分区上 `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_CODEPAGE_936=y`、`CONFIG_FATFS_API_ENCODING_UTF_8=y` 三项必须一起保留，否则 SD 上的非 ASCII 长文件名会乱码。

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
| `factory` | — | 4 MB | app |
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

## 七、启动序列（文档规划，骨架尚未完全实现）

```
1. nvs_flash_init()
2. bsp_init()                 → I2C, SPI, LEDC, PCA9557
3. hal_init_all()             → LCD, Touch, Audio, IMU, Storage, Camera, Button, IO
                                └─ lvgl_port_init() + lvgl_port_add_disp()
4. services_init()            → EventBus, Storage, Time, Audio, Net, Power, Noti
5. fw_init()                  → Theme, Asset, Window, Input, StatusBar, CtrlCenter, NotiCenter, AppMgr
6. app_register_all()         → 注册所有内置 app
7. fw_boot_animation()        → Logo 动画 1.5s
8. fw_app_mgr_launch("Home")  → 显示桌面
```

时间预算（目标到首屏 < 2.5s）见 `docs/03-design/04-data-flow.md`。

---

## 八、典型数据流

### 触摸事件
drv_ft6336_read → hal_touch 任务扫描 → lvgl_port_touch_cb → LVGL input device 派发 → app 处理

### 音频播放
app 调用 `svc_audio_play(path)` → svc_audio 任务 → svc_storage 读文件 → helix MP3 解码 → I2S DMA → ES8311 → NS4150B 功放 → 喇叭

### 配置持久化
app 调用 `svc_settings_set(key, value)` → NVS write → 触发 `SVC_EVENT_*_CHANGED` → `fw_app_mgr_broadcast` → 关心此 key 的 app 刷新 UI

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