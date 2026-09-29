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
│   ├── main.c              # app_main()，v0.5：Drivers + Peripherals + Services + Framework，启动 Home
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── drivers/                # 芯片级驱动（已完成）
├── peripherals/            # 外设抽象层（已完成）
├── services/               # 业务服务层（net / audio 已实现，MP3 / 录音 / MQTT 等未实现）
├── framework/              # UI 框架层（10 个模块 + assets/ 字体子集与开机 Logo）
├── apps/                   # 应用层（app_home / app_clock / app_settings）
├── managed_components/     # 组件管理器自动填充，按构建产物对待
├── docs/                   # 设计文档（需求 / 架构 / 详细设计）
│   ├── 01-requirements/    # 原始需求、硬件规格、PRD
│   ├── 02-architecture/    # 架构总览、分层、模块依赖
│   └── 03-design/          # Peripherals / Services / UI / App 框架 / 数据流
└── build/                  # 构建产物（在 .gitignore）
```

**关键文件说明**：

- `main/main.c` —— v0.5 启动入口：NVS → `bsp_init()`（Drivers 层：I2C0 GPIO1/2 100 kHz → LEDC 背光 GPIO42 → PCA9557 @ 0x19 → ST7789 屏 SPI3_HOST 40/41/39 80 MHz 模式 2 → FT6336 单点触摸 @ 0x38 → BOOT 键 GPIO0 → QMI8658 @ 0x6A）→ `peripherals_init_all()`（IO / Audio / LCD+LVGL / Touch / IMU / Storage / Button）→ `services_init()`（EventBus / Settings / Storage / Time / Audio / Net / Power / Notification）→ `fw_init()`（Framework 层）→ `app_register_all()` → `fw_app_mgr_launch("Home")` 显示桌面 → 挂载内置 SPIFFS。
- `main/idf_component.yml` —— 声明依赖：`idf >=5.4.0`、`lvgl/lvgl ~8.3.0`、`espressif/esp_lvgl_port ~1.4.0`、`espressif/esp_lcd_touch_ft5x06 ~1.0.7`（ES8311 音频 DAC 不用组件，见 `drivers/src/drv_es8311.c` 自实现的寄存器驱动）。
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
| `drivers/` | `pca9557`、`st7789`、`ft6336`、`qmi8658`、`es8311`、`i2c`、BOOT 按键、LEDC、BSP | 已实现（`es7210`、`gc0308` 规划中；TF 卡的 sdmmc/fatfs 挂载由 Peripherals 层的 `periph_storage` 直接用 IDF 组件完成，不单独设驱动） |
| `peripherals/` | `periph_lcd_*`、`periph_touch_*`、`periph_audio_*`、`periph_imu_*`、`periph_storage_*`、`periph_io_exp_*`、`periph_button_*` | 部分实现：LCD/Touch/Button/IMU/Storage/IO/Audio 已实现（音频仅播放）；Camera 未包含 |
| `services/` | `svc_event_bus`、`svc_settings`、`svc_time`、`svc_audio`、`svc_net`、`svc_storage`、`svc_notification`、`svc_power` | event_bus / settings / storage / time / power / notification 已实现；net（Wi-Fi STA + HTTP）与 audio（WAV / MP3 / tone 播放 + ES7210 录音）已实现；SmartConfig / MQTT / WS / OTA / 蓝牙未实现 |
| `framework/` | `fw_app_mgr`、`fw_window`、`fw_input`、`fw_theme`、`fw_asset`、`fw_ui`、`fw_statusbar`、`fw_notification`、`fw_control_center`、`fw_boot_animation` | 已实现（状态栏集成返回 / 主页 / 通知 / 控制中心按钮；中文由 assets 的 Noto 子集 14/16 px 渲染；开机画面用官方 Logo + 提示音） |
| `apps/` | `app_home`、`app_clock`、`app_settings` | 已实现这 3 个（Settings：Wi-Fi 扫描 / 免密重连 / 忘记、亮度、背光超时、主题（立即生效）、关于）；`app_music`、`app_file` 等其余 14 个规划中 |

五层目录均已建立：`drivers/`、`peripherals/`、`services/`、`framework/`、`apps/`。新增模块时遵守 `docs/02-architecture/01-layer-design.md`：

- **命名**：`drv_<chip>_*`、`bsp_*`、`periph_<dev>_*`、`svc_<svc>_*`、`fw_<mod>_*`、`app_<name>_*`。
- **返回值**：所有公开 API 返回 `esp_err_t`。
- **日志**：禁用 `printf`，统一用 `ESP_LOGI/W/E`，TAG 带层前缀（`"drv.st7789"`、`"periph.lcd"`、`"svc.audio"`、`"fw.window"`、`"app.clock"`）。
- **单向调用**：Peripherals 不能调 Services，Apps 不能调 Peripherals/Drivers，Services 不能调 Framework/Apps。
- **例外**：Peripherals / Services / Framework 中可以使用 `lvgl_port_lock/unlock`；`fw_input` 可直接注册 `periph_button` 回调（按键事件尚未接入事件总线）。
- **任务模型**：每个 Service 通常独占一个 FreeRTOS 任务；同步 API 只用于简单 setter。

---

## 四、AI 易踩的坑（按出错的代价排序）

界面尺寸、圆角、间距、配色与交互统一以 `docs/03-design/02-ui-system.md` 第 14 节《UI 视觉规范》为准；新增或改动界面照现有规格做，不要另起一套风格。

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

### 4.9 中文界面文案与字体子集

中文由 `framework/assets/font_cn14.c` / `font_cn16.c` 渲染（Noto Sans SC 栅格化，OFL 授权），是**只含界面用字的子集**。新增中文文案若出现方框，说明用到了字表外的字 —— 把该字加进 `tools/cn_chars.py` 的 `CN_CHARS` 并重新生成（源字体在 `tools/fonts/NotoSansSC-VF.ttf`，路径可省略）：

```powershell
python tools/gen_cn_font.py --sizes 14,16
```

字体统一从 `fw_asset_font_cn()`（14 px 正文）/ `fw_asset_font_cn_large()`（16 px 标题）/ `fw_asset_font_14|20|24()`（拉丁）获取，不要直接引用字体变量。开机画面用 `framework/assets/image_lckfb_logo.c`（立创官方 120×120 资源，已裁掉非 16-bit-swap 分支）。

改完界面文案后先跑一次自检，确认没有用到字表外的字（否则会显示方框）：

```powershell
python tools/check_cn_text.py
```

动态数据（如 Wi-Fi 名称、文件名）里的汉字不在字表内是正常的 —— UI 字体回退到 `font_cn_extra`（`--cs gb2312` 生成，GB2312 一级 3755 常用字，14 px / 2bpp），两条生成命令：

```powershell
python tools/gen_cn_font.py --cs gb2312 --sizes 14 --bpp 2
python tools/gen_cn_font.py --sizes 14,16
```

回退链：`font_cn14`/`font_cn16` → `font_cn_extra` → `lv_font_montserrat_14`（FontAwesome 符号）。

### 4.10 点亮背光前必须先清屏

ST7789 的 GRAM 掉电 / 复位后不会自动清空。若先开背光再等 LVGL 首帧，会短暂显示**上一次运行残留在面板里的画面**（表现为开机"先闪一下主页"）。`periph_lcd_init()` 在设置背光前先 `periph_lcd_fill(0x0000)` 整屏清黑。

复位与 CS 时序**不要动**：本板面板 RST 是 NC，官方例程的顺序是"先 `esp_lcd_panel_reset()`（此时 CS 仍为高）→ 再拉低 CS → 再 `esp_lcd_panel_init()`"。曾把"拉低 CS"挪到复位之前，SWRESET 就真正生效了，而 ST7789 复位后需要约 120 ms 才能接受新命令（IDF 内部只等 20 ms），随后的初始化命令被丢弃——表现是**开机只有背光、没有画面**。

### 4.11 开机提示音要先打成功放

`periph_audio_set_mute(false)` 之后功放 / codec 有几百毫秒的启动斜坡。若紧接着播放很短的提示音，开头会被这段斜坡吃掉（听起来"没有声音"）。`fw_boot_animation()` 先解除静音、等 150 ms，再播放 300 ms 的提示音。

### 4.12 点击请用 LV_EVENT_SHORT_CLICKED

LVGL 的 `indev_proc_release` 会**无条件**发送 `LV_EVENT_CLICKED`，只有 `LV_EVENT_SHORT_CLICKED` 才判断"无长按、无滑动"。所以界面上的"点击"处理要注册 `LV_EVENT_SHORT_CLICKED`，否则长按后松手也会触发点击（例如长按 Wi-Fi 磁贴会同时弹出清除凭据对话框并切换开关）。滑块 / 复选框仍用 `LV_EVENT_VALUE_CHANGED`。

### 4.13 换主题要重建 UI

控件配色是写死在控件上的，改调色板不会影响已创建的对象。`fw_theme_apply()` 切换 LVGL 自带主题的明暗并发布 `SVC_EVENT_THEME_CHANGED` 后，由 `fw_init` 的处理器重建状态栏、两个浮层与所有 App 的界面（App 内部页面回到初始页），从而立即生效。新增需要跟随主题的 UI 模块时，实现一个 `fw_*_rebuild()` 并在该处理器里登记。

重建必须注意以下四点，否则会崩：

- **不能在 App 事件回调里同步重建**：先把重建 `lv_async_call()` 丢到 LVGL 任务里执行，否则会删掉"正在处理事件的控件"。
- **禁止删除活动屏**：`lv_obj_del()` 删除当前活动屏时会把 `lv_disp_t.act_scr` 置为 NULL（`lv_obj_tree.c`），随后任何 `lv_scr_load*()` 都会在 `lv_obj_set_pos(lv_scr_act(), ...)` 处空指针崩溃。重建前先 `lv_scr_load()` 一块临时空屏，最后确认它已不是活动屏再删除。
- **`fw_window` 的 `s_active` 是裸指针**：旧屏被删除后它悬空，而新屏很可能复用同一地址，导致 `fw_window_switch_to()` 误判"已在目标屏"而跳过切换。切换前先 `fw_window_sync_active()`，且切换判重时同时核对 `lv_scr_act()`。
- 重建前后台 App 要保持原样：对当前前台 App 依次 `on_destroy` → `on_create` → `on_pause` → `on_start`（`on_pause` 用来退订，避免 `on_start` 重复订阅），最后再切到它的新根屏。

### 4.14 控件必须显式设色，不要依赖 LVGL 自带主题

`lv_theme_default` 会给 `lv_btn` 加 `bg_color_primary`（主色底 + **白字**）。我们把按钮底色改成 `bg_card` 之后，按钮里的标签如果自己不设 `text_color`，就会用主题给的白字：深色主题下白字落在深色卡片上看不出来，浅色主题下就是白字白底、**直接消失**（曾发生在通知中心的关闭按钮 × 和 `fw_ui_dialog()` 的按钮上）。所以：

- `lv_btn_create()` 之后，按钮内的标签一律显式 `lv_obj_set_style_text_color()`。
- 滑块（`LV_PART_MAIN` 轨道 / `LV_PART_INDICATOR` 指示条 / `LV_PART_KNOB` 圆点）、输入框、卡片、浮层同理。
- 改完界面跑 `python tools/check_ui_colors.py` 自检（扫描"创建标签但附近没有设色"的地方）。
- 浅色主题下白卡片与浅灰页面靠描边区分，卡片 / 按钮 / 浮层都要 `border_width = 1` + `border_color = fw_theme_color_border()`。

### 4.15 I2C 统一走新版 i2c_master 驱动

Drivers 层不再使用旧版 `driver/i2c.h`（`i2c_driver_install()` 会打印迁移告警，而且旧驱动与新版 `i2c_master` 不能同时占用同一个端口）：

- 总线在 `bsp_init()` 里由 `drv_i2c_bus_init()` 建一次（I2C0，GPIO1/2，100 kHz），句柄用 `drv_i2c_bus_handle()` 取。
- 芯片驱动在自己的 `init()` 里 `drv_i2c_device_add(地址, 频率, &s_dev)` 挂设备，之后用 `drv_i2c_read_reg()` / `drv_i2c_write_reg()` 读写寄存器（内部是 `i2c_master_transmit_receive()` / `i2c_master_transmit()`）。
- 触摸走 esp_lcd：`esp_lcd_new_panel_io_i2c()` 传入 `drv_i2c_bus_handle()` 时会由 `_Generic` 自动分派到 v2 实现，并且**必须显式给 `tp_io_config.scl_speed_hz`**（v2 不接受 0，v1 反而要求 0，别照抄旧例程）。
- ES8311 音频 DAC 原先依赖已废弃的 `espressif/es8311`（只能用旧驱动、新版永不支持），现由 `drivers/src/drv_es8311.c` 自实现寄存器序列（源自该组件，Apache-2.0），只做 I2C 配置，I2S 数据通路仍在 `periph_audio`。时钟拓扑固定为 MCLK = 采样率 × 256，该比例下分频系数与采样率无关，所以只有一组系数。

### 4.16 新增 / 删除源文件后必须重新配置

`idf_component_register(SRC_DIRS src)` 里的目录是 **CMake 配置期**用 glob 展开的，新增或删除 .c/.h 文件后直接 `idf.py build` 不会重新扫描。典型表现是**链接期**报 `undefined reference to xxx`（源文件明明存在，只是没进库），或反过来报某个 .obj 找不到源文件。

正确做法（改完文件结构先 reconfigure，再 build）：

```powershell
idf.py reconfigure
idf.py build
```

`framework/assets/` 这类资源目录增删文件后同理。只改已有文件内容不需要 reconfigure。

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
5. fw_init()                  → Theme, Asset, Window, AppMgr, UI, StatusBar, NotiCenter, CtrlCenter, Input
6. app_register_all()         → 注册所有内置 App
7. fw_boot_animation()        → 全屏官方 Logo 静态展示 + 单声开机提示音（约 1.7 s）
8. fw_app_mgr_launch("Home")  → 显示桌面
9. periph_storage_mount(内置) → 首屏之后挂载内置 SPIFFS（首次自动格式化）

（未实现：控制中心的蓝牙 / 手电筒 / 锁屏磁贴、状态栏亮度图标、fw_notification_clear 单条撤销）
```

时间预算（目标到首屏 < 2.5 s）见 `docs/03-design/04-data-flow.md`。

---

## 八、典型数据流

### 触摸事件
drv_ft6336 → periph_touch 扫描任务（缓存 + 手势识别）→ LVGL indev read_cb 读缓存 → LVGL input device 派发 → App 处理

### 音频播放
App 调用 `svc_audio_play(&src)` → svc_audio 的 play_task → VFS 读文件（WAV / PCM 16-bit）→ `periph_audio_write()` → I2S0 DMA → ES8311 → NS4150B 功放 → 喇叭（PA_EN 由 PCA9557.BIT1 控制）

### 配置持久化
App 调用 `svc_settings_set(key, value)` → NVS write → 触发 `SVC_EVENT_*_CHANGED` → 订阅该事件的 App 自行刷新 UI（App 在 `on_start` 订阅、`on_pause` 退订）

### 手势
periph_touch 扫描任务（swipe 识别）→ svc_power 发布 `SVC_EVENT_GESTURE_SWIPE_*` → fw_input 路由 → 通知中心 / 控制中心 / 返回

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