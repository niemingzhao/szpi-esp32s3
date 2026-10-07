# AGENTS.md — szpi-esp32s3 (SZPI-OS)

## 一、语言与开发约定

- **语言约定**：本项目所有文档（`README.md`、`AGENTS.md`、`docs/`、代码注释）默认使用简体中文，与 `docs/` 现有内容保持一致。
- **文本与术语约定**：更新任一层文档或代码注释时，逐篇核对以下写法（`00-original-requirement.md`、`01-hardware-spec.md` 是术语与硬件的基准）：
  - `I2C`（不写 `I²C`）、`I2S`、`SPI`
  - `Wi-Fi`（不写 `WiFi`）
  - `SPIFFS`（不写 `LittleFS`）
  - 叙述中用 `TF 卡`（不写 `SD 卡`）
  - 中文叙述用 `App`（`apps/`、`app_<name>` 等英文标识除外）；脚本统一叫「脚本」
  - 数字与单位之间加空格：`16 kHz`、`100 ms`、`30 FPS`
  - 不使用 emoji，不使用 `>` 引用块
  - 组件 / 头文件名用 ESP-IDF 的实际名字（如 `bt`、`mqtt`、`vfs`、`json`，不写 `esp_bt`/`esp_mqtt`/`esp_vfs`/`cJSON`）
  - 涉及硬件参数时与 `01-hardware-spec.md` 核对（触摸为单点等）
- **开发约定**：AI 助手**只负责编写代码、编写文档、操作文件**。以下操作**严禁 AI 自行执行**，必须由人类开发者完成：
  - 构建项目（`idf.py build` / `idf.py set-target` 等）
  - 烧录固件到设备（`idf.py flash` / `esptool.py` 等）
  - 监视串口输出（`idf.py monitor` / 串口工具等）
  - 任何会与真实硬件交互的动作
- AI 可以生成这些命令供人类复制执行，但不要在工具调用里发起。

### 1.1 文档更新边界

| 文档 | 可以改吗 |
|------|----------|
| `docs/01-requirements/00-original-requirement.md`、`01-hardware-spec.md` | **基准**：作为最原始文档原则上不改；仅当与实现出现事实性矛盾时按实现修正，不做范围扩写与风格重写 |
| `docs/01-requirements/02-prd.md` | **受控**：仅当人类要求、或代码变更必须同步时才改 |
| `docs/02-architecture/`、`docs/03-design/`、`AGENTS.md`、`README.md` | 随代码实现随时可更新 |

结构性的东西（分区表、关键配置项）放在可更新的架构文档（`docs/02-architecture/00-overview.md`），不要写进硬件规格（它是硬件与术语的基准）。

---

## 项目一句话定位

运行在立创·实战派 ESP32-S3 开发板（ESP32-S3-WROOM-1-N16R8）上的嵌入式固件 SZPI-OS。ESP-IDF **v6.1**，目标 `esp32s3`，xtensa-gcc。**脚本驱动的极简 OS**：核心能力（界面 / 音频 / 摄像头 / 硬件 / 文件 / 网络 / BLE / 系统信息）通过 Lua 脚本暴露给用户。单体固件，纯 C + Lua；无 CI、无主机测试。

---

## 二、仓库入口（先看这里）

```
szpi-esp32s3/
├── CMakeLists.txt          # 顶层，EXTRA_COMPONENT_DIRS 指向 main 下各层
├── partitions.csv          # 自定义分区表（factory 8M + storage 7M）
├── sdkconfig               # 自动生成，禁止手改
├── sdkconfig.defaults      # 策略文件，可改
├── main/
│   ├── main.c              # app_main()
│   ├── CMakeLists.txt
│   ├── idf_component.yml   # 组件依赖
│   ├── drivers/            # Drivers 层
│   ├── peripherals/        # Peripherals 层
│   ├── services/           # Services 层
│   ├── framework/          # Framework 层（含 assets/ 字体、图标与开机 Logo）
│   └── apps/               # Apps 层（含 src/app_common.c 注册表）
├── managed_components/     # 组件管理器自动填充，按构建产物对待
├── tools/                  # 自检脚本（check_*）、工具测试（tests/）与字体 / 图标生成
├── docs/                   # 设计文档（需求 / 架构 / 详细设计）
├── docker/                 # 构建镜像（Dockerfile，本地与 CI 共用）
├── .devcontainer/          # VS Code 开发容器
├── .github/workflows/      # CI（静态自检 + 编译）与一键发布
├── releases/               # 发布用合并固件（szpi-esp32s3.bin，随版本提交）
└── build/                  # 构建产物（在 .gitignore）
```

**关键文件说明**：

- `main/main.c` —— 启动入口：NVS → `bsp_init()`（Drivers：I2C0 GPIO1/2 100 kHz → LEDC 背光 GPIO42 → PCA9557 @ 0x19 → ST7789 屏 SPI3_HOST 40/41/39 80 MHz 模式 2 → FT6336 单点触摸 @ 0x38 → BOOT 键 GPIO0 → QMI8658 @ 0x6A）→ `peripherals_init_all()` → `services_init()` → `fw_init()` → `app_register_all()` → `fw_boot_animation()` → `fw_app_mgr_launch("Home")` → 挂载内置 SPIFFS → `svc_watchdog_arm()`。
- `main/idf_component.yml` —— 组件依赖（LVGL 9、esp_lvgl_port 2.x、esp_lcd_touch_ft5x06、esp32-camera、helix MP3、esp_websocket_client、espressif/lua 等）。ES8311 / ES7210 音频 codec 不用组件，见 `main/drivers/src/drv_es8311.c`、`drv_es7210.c` 自实现的寄存器驱动。
- `dependencies.lock` —— 组件管理器生成的锁定文件，**不提交**（在 `.gitignore` 里）、**禁止手改**。升级时改 `main/idf_component.yml` 或 `sdkconfig.defaults`，让构建工具重新生成。
- `partitions.csv` —— 自定义分区表，**不要切换到内置分区方案**。

---

## 三、5 层架构

调用方向（**禁止反向调用**）：

```
Apps / 脚本 → Framework → Services → Peripherals → Drivers → ESP-IDF/FreeRTOS
```

| 目录 | 模块 |
|------|------|
| `main/drivers/` | `bsp_init`、`i2c`（新版 i2c_master + 寄存器读写）、`pca9557`、`st7789`、`ft6336`、`qmi8658`、`es8311`、`es7210`、`camera`（DVP，兼容 GC0308 / GC2145）、`key`（BOOT 键）、`ledc`（背光） |
| `main/peripherals/` | `periph_lcd`、`periph_touch`、`periph_audio`、`periph_imu`、`periph_storage`、`periph_camera`、`periph_io_exp`、`periph_button`、`periph_ext`（外扩 GPIO / PWM / I2C / UART / ADC / CAN） |
| `main/services/` | `svc_event_bus`、`svc_settings`、`svc_storage`、`svc_time`、`svc_audio`、`svc_net`、`svc_bt`、`svc_mqtt`、`svc_ws`、`svc_power`、`svc_imu`、`svc_io`、`svc_camera`、`svc_sysinfo`、`svc_web`、`svc_identity`、`svc_watchdog` |
| `main/framework/` | `fw_app_mgr`（原生 App）、`fw_script`（Lua 运行时 + 脚本管理 + 绑定）、`fw_window`、`fw_input`、`fw_theme`、`fw_asset`、`fw_ui`、`fw_statusbar`、`fw_pairing`、`fw_boot_animation` |
| `main/apps/` | `app_home`（桌面，不算在 22 个里）+ 22 个内置 App：`app_scripts`、`app_clock`、`app_calendar`、`app_weather`、`app_wifi`、`app_bt`、`app_display`、`app_sound`、`app_file`、`app_editor`、`app_calc`、`app_download`、`app_music`、`app_recorder`、`app_camera`、`app_image`、`app_stopwatch`、`app_timer`、`app_imu`、`app_perf`、`app_log`、`app_about` |

新增模块时遵守 `docs/02-architecture/01-layer-design.md`：

- **命名**：`drv_<chip>_*`、`bsp_*`、`periph_<dev>_*`、`svc_<svc>_*`、`fw_<mod>_*`、`app_<name>_*`。
- **返回值**：所有公开 API 返回 `esp_err_t`。
- **日志**：禁用 `printf`，统一用 `ESP_LOGI/W/E`，TAG 带层前缀（`"drv.st7789"`、`"periph.lcd"`、`"svc.audio"`、`"fw.script"`、`"app.clock"`）。
- **单向调用**：Peripherals 不能调 Services，Apps / 脚本不能调 Peripherals/Drivers，Services 不能调 Framework/Apps。
- **例外**：Peripherals / Services / Framework 中可以使用 `lvgl_port_lock/unlock`；`fw_input` 可直接注册 `periph_button` 回调（同时把按键事件转发到 `SVC_EVENT_KEY` 供脚本订阅）；脚本对硬件的访问一律经 `svc_io`。
- **任务模型**：每个 Service 通常独占一个 FreeRTOS 任务；同步 API 只用于简单 setter。

---

## 四、AI 易踩的坑（按出错的代价排序）

界面尺寸、圆角、间距、配色与交互按 `docs/03-design/02-ui-system.md` 第 12 节《UI 视觉规范》执行；新增或改动界面照现有规格做，不要另起一套风格。

### 4.1 LVGL 调用必须加锁

在非 LVGL 任务里（触摸扫描任务、App 回调、Services、Framework 初始化）调用任何 LVGL API，**必须**用 `lvgl_port_lock(0)` / `lvgl_port_unlock()` 包起来。`lvgl_port` 用的是递归互斥锁，因此在 LVGL 事件回调内再次加锁是安全的。

全局浮层挂在 `lv_layer_top()` 上时，必须先 `lv_obj_set_clickable(lv_layer_top(), false)`：`lv_obj` 默认可点，否则浮层会吞掉全屏触摸（LVGL 命中顺序为 layer_sys → layer_top → 当前屏）。

### 4.2 LVGL 绘制缓冲放内部 DMA 内存，不放 PSRAM

`lvgl_port_display_cfg_t.flags.buff_dma = true` + `buff_spiram = false`（10 行 = 320×10×2 = 6.4 KB，单缓冲）。

原因：ESP32-S3 的 `esp_ptr_dma_capable()` 只覆盖内部 DRAM（`SOC_DMA_LOW~SOC_DMA_HIGH` = 0x3FC88000~0x3FD00000），**PSRAM 不在其中**，所以绘制缓冲放 PSRAM 时 SPI 驱动会为**每一笔 flush** 临时 `heap_caps_aligned_alloc(..., MALLOC_CAP_DMA)` 一块同样大小的内部回弹缓冲（本板 `max_transfer_sz` 是整屏，一次 flush 就是 10 KB 级）。Wi-Fi / BLE 起来后这笔分配会失败 → `panel_io_spi_tx_color: spi transmit (queue) color failed` → 刷屏失败；而 `esp_lvgl_port` 自带的 flush 回调忽略返回值、也不调 `lv_display_flush_ready()`，单缓冲下 LVGL 会死等这块缓冲 —— 表现是 LVGL 任务占满 CPU、界面永久卡住、事件总线任务被饿死触发看门狗。`periph_lcd_flush_cb()` 覆盖了它的 flush 回调，失败时补一次 `lv_display_flush_ready()`（最多丢一帧，绝不死锁）并打印错误码。

### 4.3 LVGL 9 与 v8 的差异

本项目用 **LVGL 9**（`LV_COLOR_16_SWAP` 已移除、`lv_disp_*`→`lv_display_*`、`lv_img_*`→`lv_image_*`、`lv_btn_*`→`lv_button_*`、`LV_MEM_CUSTOM`→`LV_USE_STDLIB_MALLOC`）。几条常被踩的：

- **RGB565 字节交换**：LVGL 9 不再有 `LV_COLOR_16_SWAP`，在显示 flush 回调里做（`LV_COLOR_FORMAT_RGB565_SWAPPED` 或 `lv_draw_sw_rgb565_swap()`）。
- **内存分配**：`sdkconfig.defaults` 里设 `CONFIG_LV_USE_CLIB_MALLOC=y`（choice 里选中 CLIB，LVGL 的 `LV_USE_STDLIB_MALLOC` 随之等于 `LV_STDLIB_CLIB`，去走 IDF 堆，受 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 影响）。注意 `CONFIG_LV_STDLIB_CLIB` 只是个 `int` 常量、**不是**开关，写成 `=y` 会被 Kconfig 静默忽略而回落到内置 TLSF 分配器。不要再用 `LV_MEM_CUSTOM`。
- **图片解码**：PNG 是 `LV_USE_LODEPNG`、JPEG 是 `LV_USE_TJPGD`（旧版叫 `LV_USE_PNG`）。
- **LVGL 9.6 废弃了通用 flag 接口**：`lv_obj_add_flag` / `lv_obj_clear_flag` / `lv_obj_remove_flag` / `lv_obj_has_flag` / `lv_obj_set_flag` 都改成专用 setter，如 `lv_obj_set_scrollable(o, false)`、`lv_obj_set_clickable(o, true)`、`lv_obj_set_hidden(o, true)`、`lv_obj_is_hidden(o)`。用户自定义 flag（`LV_OBJ_FLAG_USER_1`）没有专用 setter，改用 `lv_obj_set_user_data()` / `lv_obj_get_user_data()` 传标记（对象由 `lv_malloc_zeroed` 分配，`user_data` 初值必为 NULL）。`lv_timer_t` 已是不可见类型，读它的 data 要用 `lv_timer_get_user_data(t)`，不能写 `t->user_data`。
- **`lv_obj_remove_style_all()` 会连本地样式一起删掉**：`lv_obj_set_width/height()`（以及 `lv_obj_set_style_*`）写的都是对象的本地样式，先 `set_size` 再 `remove_style_all` 会把尺寸打回类默认值（`lv_obj` 是 `LV_DPI_DEF` = 130×130），表现为网格里冒出一堆巨型空对象、后面的控件被挤出屏幕。要么先 `remove_style_all` 再 `set_size`，要么干脆别用它（改设 `bg_opa = TRANSP` / `border_width = 0` 就行）。
- **Lua 的 `lualib.h` 预声明了标准库入口符号**（`luaopen_io` / `luaopen_os` / `luaopen_string` / `luaopen_math` 等）：`fw_script.c` 里脚本模块的 C 入口不能与它们同名，否则「static declaration follows non-static declaration」。我们的 `io` 模块入口因此叫 `luaopen_io_hw`。
- **工具链实际是 GCC 15.2**（`esp-15.2.0_20251204`，`-std=gnu23`），4.18 那批降级清单在它上面依然适用。
- 上述选项在 ESP-IDF 下经 lvgl 组件的 Kconfig 配置。
- **逐行解码的图片不能被缩放绘制**：`lv_draw_image.c` 的 `img_decode_and_draw()` 只在 `decoder_dsc->decoded != NULL`（整幅已解码）时才做缩放 / 旋转绘制；"部分解码"的图片（实现了 `decoder_get_area` 的，如 BMP、JPEG）一旦走进缩放分支就直接 `return`（日志 `Partially decoded images cannot be transformed`），表现是**放大后画面全空**（1:1 不触发变换，照常显示）。只有 `open` 阶段整幅解完的解码器（PNG / lodepng）才 `decoded != NULL`。**开 LVGL 图片缓存（`CONFIG_LV_CACHE_DEF_SIZE`）解决不了这个问题**：`lv_image_decoder_open()` 只在解码器自己整幅解完时才把图放进缓存，逐行解码的解码器从不入缓存。要让这类格式能缩放，得让 LVGL 拿到一张"整幅已解码"的图 —— 图库 App 的做法是自己把 BMP 解成 RAM 里的 RGB565，用 `lv_image_dsc_t`（`LV_IMAGE_SRC_VARIABLE`，`header.magic = LV_IMAGE_HEADER_MAGIC`、`cf = LV_COLOR_FORMAT_RGB565`、`stride = w * 2`）交给 `lv_image_set_src()`，`lv_bin_decoder` 就把它当已解码图片直接用；JPEG 同样是逐行解码但没法自己解，图库只按原始大小显示。
- **缩放用 `LV_IMAGE_ALIGN_STRETCH` + 对象尺寸**：`lv_image_set_scale()` 在对齐是 STRETCH / CONTAIN / COVER 时**直接返回**（这些模式下 scale 由 `update_align()` 按对象尺寸反推），所以"先适应屏幕再按倍数放大"不能靠它，要么自己算 scale 配 `LV_IMAGE_ALIGN_CENTER`，要么把对象尺寸设成目标像素尺寸配 STRETCH。
- **图片缓存要开够大，否则大图每帧重解**：`CONFIG_LV_CACHE_DEF_SIZE` 默认 0（关），而整幅解码的结果是 ARGB8888（4 字节/像素）—— 缓存放不下时**每一帧都要重新解码**，图库打开 818×682 的 PNG（解码 2.2 MB）就是这么卡成幻灯片的（实测）。本项目设 **4 MB**（够放 1024×768 的 ARGB8888 = 3 MB；PSRAM 8 MB，缓存是按需分配、不预占）。图库还会在打开前用 `lv_image_decoder_get_info()` 拿尺寸，**超过 `IMG_MAX_DECODE_PX`（1024×768）直接提示「图片太大」不开**，免得去解一个缓存放不下的大图。另外注意：**`lv_image_decoder_dsc_t` 在公开头里只有前向声明**（完整定义在 LVGL 私有的 `src/image/lv_image_decoder_private.h`），所以 **App 里用不了 `lv_image_decoder_open()` / `get_area()` 这套逐区解码 API** —— 想自己把图解进 RAM 只能另起画布或包一层 Framework，用图片缓存才是官方路子。**JPEG 只支持基线（SOF0/SOF1）**，渐进式（SOF2）在 `jd_prepare` 就失败 —— 网上很多图是渐进式的，图库会提示「只支持基线 JPEG」，不然只剩一个"打不开"的方框。
- **容器里放大一张图，摆放方式必须显式改成 `LV_ALIGN_TOP_LEFT`**：`lv_obj_center()` 写的是对齐方式（style 的 align / x / y），而 `lv_obj_set_pos()` **只改 x / y、不改对齐** —— "先居中、再放大"时对象比容器大，仍按居中摆（位置是负的），而滚动量停在 0 已经是边界，**左边 / 上边那一截永远拖不出来**（图库查看器踩过）。放大分支要用 `lv_obj_align(obj, LV_ALIGN_TOP_LEFT, 0, 0)`。重排之前还要 `lv_obj_scroll_to(obj, 0, 0, LV_ANIM_OFF)`：摆放位置是按当前滚动量算的（`lv_obj_move_to()` 会减掉 `lv_obj_get_scroll_x()`），带着旧偏移重新摆放会把图摆到屏幕外；`lv_obj_set_scrollable(obj, false)` 也**不会**帮你把滚动量归零。
- **压在图上、只做装饰的悬浮层要 `lv_obj_set_clickable(obj, false)`**：`lv_obj` 默认可点，透明的整宽底栏 / 信息胶囊会把落在它上面的拖动整口吃掉，表现是"有些位置拖不动、拖不到边"。里面的按钮是独立的可点子对象，不受影响（命中测试先找子对象，父对象不可点只是自己不吞）。
- **设了 `max_length` 的文本域不能用 `lv_textarea_set_text()` 灌大段内容**：LVGL 的 `lv_textarea_set_text()` 只在 `max_length == 0` 且没设 `accepted_chars` 时走「整块 `lv_label_set_text()`」（`lv_textarea.c` 里那个 `if(accepted_chars || max_length)` 分支）；一旦设了上限（编辑器是 `ED_MAX_TEXT`），它就改成**逐字符插入**，每插一个字都要 `lv_text_get_encoded_length()` 全扫一遍当前文本、让包裹标签整幅重排、再算一次光标位置 —— 整体是 O(n²) 且常数很大（中文字体要逐字测量）。编辑器能开的文件上限 16 KB，几万个字符这么插下去会把 LVGL 任务卡住很久，而卡住期间界面一次都不重绘，看起来就像"编辑框里什么也没有"。灌内容前后临时把上限清掉即可（`lv_textarea_set_max_length(ta, 0)` → `lv_textarea_set_text()` → 再设回上限），上限只用来管键盘输入。

### 4.4 硬件细节：PCA9557 控制的不只是 IO

| 信号 | 实际位置 |
|------|----------|
| LCD CS | **PCA9557.BIT0**（不是 GPIO）—— `esp_lcd_panel_init` 前要 `drv_pca9557_set_pin(DRV_PCA9557_LCD_CS, 0)` 拉低 |
| 音频 PA_EN | PCA9557.BIT1 |
| 摄像头 PWDN | PCA9557.BIT2 |
| LCD RST | **NC**（靠 `esp_lcd_panel_reset()` 软件复位） |
| 触摸 INT | **NC**（轮询模式） |
| 触摸 RST | **NC** |

完整引脚 / I2C 表见 `docs/01-requirements/01-hardware-spec.md`。

- 触摸为单点（FT6336），只支持点击 / 长按，不支持多指与滑动手势。
- GPIO33~37 被八线 PSRAM 占用，不可用；GPIO26~32 为模组内 Flash/PSRAM，未引出。
- UART0（GPIO43/44）接 CH340K，用于下载与串口调试。
- **相机 SCCB 必须复用 I2C0**（`drv_camera.c` 里 `.pin_sccb_sda = -1` + `.sccb_i2c_port = 0`）：SCCB 与触摸 / IMU / PCA9557 同挂在 GPIO1/2。若照默认把引脚填进去，esp32-camera 会 `SCCB_Init()` **新建一条 I2C1** 接管 GPIO1/2 —— 相机一打开，触摸、IMU、IO 扩展全部失联，而且退出 App 也不恢复（I2C0 的引脚路由已经丢了，只能重启）。`pin_sccb_sda = -1` 时它走 `SCCB_Use_Port()` → `i2c_master_get_bus_handle()` 复用现成总线，退出时也不会删掉我们的总线。

### 4.5 配置文件的修改边界

- `sdkconfig` 自动生成，**禁止手改**。
- `sdkconfig.defaults` 是策略文件，可以改。
- `dependencies.lock` 禁止手改。
- `partitions.csv` 自定义表，不要切到内置方案。

### 4.6 蓝牙协议栈是 Bluedroid，不是 NimBLE

`CONFIG_BT_BLUEDROID_ENABLED=y`，`sdkconfig` 里 NimBLE 显式关闭。引入 NimBLE 专有示例前必须重新评估。

**只用 BLE 4.2**（`CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y` 且 `# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set`，两行必须一起写）。IDF 默认把 5.0 特性打开，而扩展广播 / 周期广播会明显增加 BLE 控制器内存：本板实测会在 `svc_bt_init()` 里打印 `BLE_INIT: Malloc failed`、随后控制器断言并重启循环。4.2 下 `esp_ble_gap_start_advertising()` 这类 4.2 接口可用、扩展广播不可用；这些 API 本身就在 `esp_gap_ble_api.h` 的 `#if (BLE_42_FEATURE_SUPPORT == TRUE)` 里。

BLE 还有**内存余量**要求：控制器初始化要一块 30 KB 连续内存（不足时断言在 `emi.c`），主机（Bluedroid）的 `btu_workqueue`、任务栈、队列又**只能用内部 RAM**（FreeRTOS 的对象不能放 PSRAM）。实测把 BLE 放在首屏之后（Wi-Fi / LVGL / 音频已占过内部 RAM）时，同一份固件不同次启动会随机在控制器侧（`BLE_INIT: Malloc failed`）或主机侧（`Bluedroid Initialize Fail` / `Unable to allocate resources for bt_workqueue`）失败。

**结论：`svc_bt_init()` 必须放在 `services_init()` 的早期（Storage 之后、Time / Wi-Fi 之前）**，趁内部内存最干净时初始化；代价是首屏时间增加约 0.4 s。配套的 `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`（把主机侧动态数据放到 PSRAM；控制器的分配固定用 `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`，不受它影响）+ `CONFIG_BT_ACL_CONNECTIONS=2` 继续保留。

**`CONFIG_BT_CTRL_BLE_MAX_ACT` 保持 IDF 默认 6，不要下调**：这个值（`ble_max_act`）同时决定控制器"环境内存池"的大小。实测改成 2 之后，`esp_ble_gap_start_advertising()` 被控制器以 **HCI `0x200a` status `0x07` Memory Full** 拒收（`btm_ble_write_adv_enable_complete failed`），表现为 BLE App 显示"广播中"但手机搜不到设备。

这个失败还带**时序性**：同一份固件不同次启动，有时控制器断言（`emi.c`）、有时主机返回失败，甚至"手动点一下界面"改变了 LVGL / 音频的时序就不复现 —— 本质是与 Wi-Fi / SNTP / 音频**同时分配**内部内存的竞态，不能靠"晚点再重试"绕过。`svc_bt_init()` 会先打印 `init: dma-internal heap free=... largest=...`，其中 **largest（最大连续块）** 才是控制器 30 KB 请求能否满足的关键。

`svc_bt_init()` 在控制器初始化前调用 `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` 把经典蓝牙的内存还给堆；`esp_bluedroid_init/enable` 声明在 `esp_bt_main.h`（不是 `esp_bluedroid_api.h`）。

BLE HID 设备模拟也在 `svc_bt` 内：`svc_bt_hid.c/h`（对外 API：初始化、键鼠 / 消费类报告）、`svc_bt_hid_dev.c/h`（GATT 服务本体与属性表）、`svc_bt_hid_send.c/h`（报告发送）、`svc_bt_hid_report.c/h`（报告构造与用法值），符号统一 `svc_bt_hid_*` / `SVC_BT_HID_*`，TAG 统一 `svc.bt.hid`。**Bluedroid 只允许一个 GATTS 回调**（GAP 同理），所以回调统一由 `svc_bt` 注册，事件再按 app_id / gatts_if 转发给 HID；将来新增 GATT 服务必须走这条"一个回调 + 分发"的路，否则后注册的会覆盖先注册的。HID 报告要等配对（加密）完成才能发。

中心角色（主动连外设）在 `svc_bt_central.c/h`：连接 / 断开、服务发现，以及按「服务 UUID + 特征 UUID」读 / 写特征与订阅通知（句柄每次连接都会变，UUID 稳定），符号统一 `svc_bt_central_*` / `SVC_BT_CENTRAL_*`，TAG 统一 `svc.bt.central`。**GATTC 回调同样只有一个**，所以也由 `svc_bt` 注册后转发（`svc_bt_central_gattc_event()`），与 HID 的 GATTS 分发同理。几条容易踩的：

- 操作全是异步的，结果走 `svc_bt_central_register_cb()` 的回调；回调在 Bluedroid 的 BTC 任务（栈小）里，界面要自己用 `lv_async_call()` 把更新转回 LVGL 任务，不要在回调里直接调 LVGL。
- 扫描结果里的 `addr_type` 必须原样传给 `svc_bt_central_connect()`：手机等设备用随机地址，传 `BLE_ADDR_TYPE_PUBLIC` 会直接连不上。
- `esp_ble_gattc_register_for_notify()` 传的是**特征值句柄**（不是 CCCD 句柄），注册成功后要自己 `esp_ble_gattc_get_descr_by_char_handle()` 找出 CCCD 并写 1（通知）/ 2（指示）。
- 同一时刻只允许一次连接尝试：`svc_bt_central_connect()` 在"连接中"时返回 `ESP_ERR_INVALID_STATE`，另有 **8 s** 超时兜底。实测连点几台设备会同时挂多个未完成请求，把 Bluedroid 的 GATTC 连接槽打满（串口报 `BT_GATT: Max TCB for gatt_if [N] reached.` + `Connection open failure`），之后**所有**连接都一直失败。
- 中心角色与从机链路互不影响，靠 `CONFIG_BT_ACL_CONNECTIONS=2` 支撑（不要下调，见上文 BLE 内存那节）。
- **配对参数是"安全连接 + MITM + 绑定"，IO 能力 `ESP_IO_CAP_KBDISP`**（在 `svc_bt_hid_init()` 里全局设一次，两个角色共用）：这样才能支持"两端比对同一串 6 位码"和"对端显示码、板子输入码"。协议栈要用户确认时会发 `ESP_GAP_BLE_NC_REQ_EVT` / `ESP_GAP_BLE_PASSKEY_REQ_EVT`，`svc_bt` 把它们转成 `SVC_EVENT_BT_PAIR_PROMPT`，由框架的 `fw_pairing` 弹全局提示（挂 `lv_layer_top()`，**不能只做在蓝牙 App 里** —— 广播在离开 App 后仍然有效）；用户确认后走 `svc_bt_pair_reply()`。注意：**不支持 SC + MITM 的对端（很多音箱等消费设备）会配对失败，而且协议栈随后会把链路断掉**（日志里 `bta_dm_ble_smp_cback remove bond` + `BTM_GetSecurityFlagsByTransport ... failed` + `rsn:0x13`，之后 `auth failed (reason 0x55/0x66)`）。
- **中心角色只对"已绑定过"的对端主动 `esp_ble_set_encryption()`**（`central_is_bonded()` 查绑定列表；重新加密是即时的、不弹配对），新设备不主动配对 —— 不需要加密的设备否则会被"强行配对失败"连带断链、刷一堆 BTM 错误日志，而需要加密的设备会自己发 Security Request（`svc_bt` 的 GAP 回调自动接受）或在我们访问受保护属性时由协议栈触发。**兜底**：服务发现 / 读取返回 `ESP_GATT_INSUF_AUTHENTICATION` / `INSUF_ENCRYPTION` / `INSUF_KEY_SIZE` 时，`central_request_encryption()` 对该对端补一次主动加密（每次连接只补一次，见 `s_enc_requested`），配对成功后 App 重新发现一次服务。配对结果经 `ESP_GAP_BLE_AUTH_CMPL_EVT` → `svc_bt_central_gap_event()`（借 `svc_bt` 的 GAP 回调转发，**不要**再注册第二个 GAP 回调）→ `SVC_BT_CENTRAL_EVT_AUTH_DONE` 上报。

两个**初始化时序 / 参数**坑（都曾表现为"状态停在 `state -> 1`、手机搜不到设备"）：

- HID 事件的转发开关必须**在注册 app 之前**置位。`esp_ble_gatts_app_register()` 是异步的，BTC 任务优先级又高于调用者，REG 事件常常在函数返回前就回调进来；若此时开关还没打开，事件被丢弃，服务就不会创建。
- 广播数据里的 **`service_uuid_len` 必须是 16 的整数倍**：`esp_ble_gap_config_adv_data()` 里有 `if (service_uuid_len & 0xf) return ESP_ERR_INVALID_ARG;`，直接传 2 字节的 16 位 UUID 会被拒收。要广播 16 位服务 UUID（如 HID `0x1812`）必须用 16 字节的 128 位形式（Bluetooth base UUID，`[12]/[13]` 就是那两个字节）。
- 广播 / 扫描**不依赖 GAP 完成事件**：把 `esp_ble_gap_config_adv_data()` / `esp_ble_gap_set_scan_params()` 返回 `ESP_OK` 当作就绪，顺序由 BTA / BTU 单队列保证；`ADV_START_COMPLETE` 到达时仍用于确认，失败则回退并按 3 s / 最多 5 次重试。
- **连接 / 断开事件会按"每个注册过的 GATT app"各回调一次**，所以 `gatts_cb` 里的连接 / 断开处理必须按 `gatts_if` 过滤，`svc_bt_note_conn()` 要幂等。
- **`esp_ble_gattc_open()`（中心角色连出去）也会给每个 GATTS app 发一次 `ESP_GATTS_CONNECT_EVT` / `DISCONNECT_EVT`**（IDF 的 `esp_ble_gattc_open()` 说明第 2 / 3 条写明）。从机侧的 `gatts_cb` 因此要把它滤掉：`CONNECT` 用 `svc_bt_central_owns_link()` 判（`svc_bt` 与 HID 都靠它），否则 HID 会把"我们连出去的链路"当成主机连进来，对它强行 `esp_ble_set_encryption()`，不支持 SC + MITM 的设备配对失败 → Bluedroid 删 bond、清临时认证、把链路断掉，串口被 `BTM_GetSecurityFlagsByTransport ... failed` 刷屏；中心角色与 HID 还会对同一条链路各发一次加密（`E BT_APPL: earlier enc was not done for same device`）。**`DISCONNECT` 不要按地址滤**：同一台设备可能既连着我们的 HID、又被主机页连出去（同一条 ACL 共用），按地址滤会把断开也吞掉、从机页永远停在"已连接"；改成比对 `conn_id`（`s_connected && param->disconnect.conn_id == s_conn_id` 才放行）。
- **`svc_bt_central_owns_link()` 不能只看 `s_connecting`**：连接尝试被取消 / 超时后标志就清了，而那条 ACL 的 GATTS `CONNECT` 可能随后才到，会被从机侧误当成"主机连进来"（表现为主机页连接后从机页显示"已连接"）。所以用 `s_link_owned` 一直记到链路真正断开 / `OPEN` 失败 / 超时为止。中心自己的 `GATTC_CONNECT` / `DISCONNECT` / `OPEN` 也都用它判归属，对端连进来的链路不会覆盖或清掉中心状态；`OPEN` 失败与"还没连上就先来 `rsn=0x100` 断开"这两条路径只通知一次，避免界面把一次失败弹两遍。
- **认证要求按角色切换**：中心角色连出去时用 `ESP_LE_AUTH_REQ_SC_BOND`（安全连接 + 绑定，**不要 MITM**）—— 这里连的是任意设备，要求 MITM 会让不支持它的消费设备配对失败、进而被协议栈断链；主机连进来（HID 从机角色）时在 `gatts_cb` 里设回 `ESP_LE_AUTH_REQ_SC_MITM_BOND`，配对提示（数字比对 / 输码）才能用。`svc_bt_hid` 里若把"配对失败但链路不受影响"写进注释，那是不对的：实测协议栈随后会断链。

排查入口：启动日志里的 `callbacks: gatts=... gap=... match=x/y`（用 `esp_ble_gatts_get_callback()` / `esp_ble_gap_get_callback()` 读真正生效的回调指针）、`selftest: state=... adv(ready/active) scan_ready=...`（`svc_bt_init()` 后 2 s 自动打印）。

扫描（中心角色）的三条关键日志：`scan requested (N s)`（请求已发出）、`first device: xx:xx:... rssi=..`（本轮第一条结果，证明真的收到了广播包）、`scan done: N device(s)`（扫描结束与总数）。收不到设备时按这三条定位：只有 `scan requested` 说明扫描没拿到射频（先确认不是在和 Wi-Fi / 广播抢时隙），连 `scan done` 都没有说明事件没回来（服务侧会补发，见下）。另外扫描参数用 `BLE_SCAN_DUPLICATE_DISABLE`（与 IDF 例程一致）：去重交给主机侧，避免控制器侧残留的重复表把后续扫描的设备挡掉。

**扫描时长不要交给控制器**：实测控制器按 `duration` 自己到点停时**不会**给主机发 `ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT`，应用收不到 `SCAN_DONE` 就会一直停在"正在扫描…"；如果事后另调一次 `esp_ble_gap_stop_scanning()` 补事件，BTM 那边扫描早已不是 active，会打出 `E BT_BTM: BTM_BleScan scan not active` + `W BT_APPL: bta_dm_ble_scan stop scan failed` 两条错误日志。正确做法是 `esp_ble_gap_start_scanning(0)`（持续扫描）+ 自己定时停：`svc_bt_scan_start()` 里用 `bt_scan_stop_schedule()` 起一个一次性 `esp_timer`，到点显式 `esp_ble_gap_stop_scanning()` —— 这时扫描一定还是 active，BTM 正常走完停止流程，`SCAN_DONE` 必定送达且日志干净。

### 4.7 FATFS 三项必须同时保留

`storage` 分区上 `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_CODEPAGE_936=y`、`CONFIG_FATFS_API_ENCODING_UTF_8=y` 三项必须一起保留，否则 TF 卡上的非 ASCII 长文件名会乱码。

**往 TF 卡写"待在 flash 里的数据"必须先过一块内部 DMA 缓冲。** FATFS 遇到"扇区对齐、且这一次要写满至少一个扇区"的写入时，会把**调用者的缓冲区指针**直接交给底层（`ff.c` 的 `f_write` → `disk_write` → `sdmmc_write_sectors`），而 ESP32-S3 的 SDMMC DMA 读不到映射在 flash 里的地址（`sd_host_check_buffer_alignment()` 只区分内部 RAM 和 PSRAM，**不认 flash**，所以它会把 flash 指针当"对齐"放行）。结果非常隐蔽：**文件长度完全正确、`fwrite` 返回全量字节、`fclose` 也不报错，写进卡里的内容却是一片 0**。表现是"文件大小正常、打开却是空的"：编辑器显示空白，Lua 报 `xxx.lua:1: unexpected symbol`（首字节是 NUL），`fgets` 读头部也读不到东西。踩过的是 `release_samples()` —— 内置示例脚本 / `脚本接口参考.txt` 是构建期 `EMBED_FILES` 嵌进来的资源（都在 flash 的 `.rodata.embedded` 里），长度超过 512 字节的那几个全军覆没，459 字节的 `counter.lua`（不足一扇区，走 FATFS 自己的窗口缓冲）反而正常，所以看着像"只有大文件坏"。`svc_storage_write()` / `svc_storage_append()` / `svc_storage_copy()` 现在统一走 `storage_write_all()`：内部 RAM 直接写，其余（flash / PSRAM，S3 上两者映射在同一段地址、指针上分不出）按 1 KB 拷进 `MALLOC_CAP_DMA` 缓冲再写。**别在 Services 层以外自己 `fopen`/`fwrite` 写 flash 数据**；栈上的小结构体（WAV 头 44 字节那种）不足一扇区，是安全的。

### 4.8 中文界面文案与字体

中文由 `main/framework/assets/fw_fonts.c` 里的三套字体渲染（Noto Sans SC 栅格化，LVGL 9 字体格式），声明在 `include/fw_fonts.h`：

- `font_cn14` / `font_cn16`：**GB2312 全集**（6763 汉字 + 682 符号），14 / 16 px / 4 bpp，界面正文与标题；
- `font_cn_extra`：**GBK 全集**（20902 汉字），14 px / 2 bpp，前两套查不到的字回退到它。

回退链：`font_cn14`/`font_cn16` → `font_cn_extra` → `lv_font_montserrat_14`（Latin + LVGL 内置符号）。常规界面文案不需要维护字表；GBK 以外的字（生僻字、emoji）会显示成方框，确实需要就扩大覆盖范围（改 `tools/gen_fw_fonts.py` 里的字符集）后重新生成：

```powershell
python tools/gen_fw_fonts.py
```

字体统一从 `fw_asset_font_cn()`（14 px 正文）/ `fw_asset_font_cn_large()`（16 px 标题）/ `fw_asset_font_14|20|24|32()`（拉丁）获取，不要直接引用字体变量。开机画面用 `include/fw_logo.h`（实现在 `assets/fw_logo.c`）。

**拉丁字体（Montserrat）也挂了中文回退**：LVGL 内置的 Montserrat 只有基本拉丁加少量符号（°、• 等），`×` `÷` 这类字符查不到字形就显示成方框（计算器的运算符键踩过：`fw_asset_font_20()` + `×` → 方框，而 `check_cn_text.py` 只看"字符串能不能被字体覆盖"，中文字体是覆盖的，所以查不出来）。`fw_asset_font_14/20/24/32()` 返回的是这几套字体的**运行时副本**，`fallback` 指向最接近字号的中文字体（14 → `font_cn14`，20 及以上 → `font_cn16`）—— 用拉丁字体同样能显示 × ÷ 等符号（回退字形按中文字体的字号画）。给副本指回退时**不能成环**：中文链是 `font_cn14`/`font_cn16` → `font_cn_extra` → `lv_font_montserrat_14`，指到前两套都不会绕回带回退的副本。

**字体 cmap 的 `range_length` 必须含端点**：LVGL 查表用的是 `rcp < range_length`（`lv_font_fmt_txt.c`），生成脚本里的 `range_length` 要写 `last - first + 1`。写成 `last - first` 时**码点最大的那个字永远查不到字形**，配合 `CONFIG_LV_USE_FONT_PLACEHOLDER=y` 就显示成方框（踩过的例子：字表里码点最大的是 `？` U+FF1F，日历"设为今天？"对话框里的问号就是方框）。`tools/check_cn_text.py` 会检查这一条，报错就重新跑 `python tools/gen_fw_fonts.py`。

### 4.9 点亮背光前必须先清屏

ST7789 的 GRAM 掉电 / 复位后不会自动清空。若先开背光再等 LVGL 首帧，会短暂显示**上一次运行残留在面板里的画面**（表现为开机"先闪一下主页"）。`periph_lcd_init()` 在设置背光前先 `periph_lcd_fill(0x0000)` 整屏清黑。

复位与 CS 时序**不要动**：本板面板 RST 是 NC，官方例程的顺序是"先 `esp_lcd_panel_reset()`（此时 CS 仍为高）→ 再拉低 CS → 再 `esp_lcd_panel_init()`"。曾把"拉低 CS"挪到复位之前，SWRESET 就真正生效了，而 ST7789 复位后需要约 120 ms 才能接受新命令（IDF 内部只等 20 ms），随后的初始化命令被丢弃——表现是**开机只有背光、没有画面**。

### 4.10 开机提示音要先打成功放

`periph_audio_set_mute(false)` 之后功放 / codec 有几百毫秒的启动斜坡。若紧接着播放很短的提示音，开头会被这段斜坡吃掉（听起来"没有声音"）。`fw_boot_animation()` 先解除静音、等 150 ms，再播放 300 ms 的提示音。

这段斜坡对**所有短提示音**都成立：`svc_audio` 的 `play_tone()` 在解除静音后统一等 `AUDIO_AMP_SETTLE_MS`（150 ms）再写 PCM，所以倒计时到点、开机这些"响一声 / 响几声"的提示音都不会被斜坡吃掉。以后新增短提示音直接调 `svc_audio_play_tone_async()` 即可，App 侧不要自己算这段等待；也因此**两声提示音的间隔要大于（150 ms + 音长）**，否则后一条会覆盖前一条还没播完的请求（音频命令队列深度为 1，用 `xQueueOverwrite` 发送）。

**开机提示音不能无条件调 `svc_audio_set_mute(false)`**：静音状态是持久化的（`sys/muted`），开机时 `svc_audio_init()` 刚读回来，这里再解除静音会把它写回 0 —— 表现是"在「声音」里设了静音，重启后又变成没静音"。正确做法是先 `svc_audio_get_mute()`，静音就整段跳过提示音（也不动状态），没静音才解除静音 + 播放。

### 4.11 点击请用 LV_EVENT_SHORT_CLICKED

LVGL 的 `indev_proc_release` 会**无条件**发送 `LV_EVENT_CLICKED`，只有 `LV_EVENT_SHORT_CLICKED` 才判断"无长按、无滑动"。所以界面上的"点击"处理要注册 `LV_EVENT_SHORT_CLICKED`，否则长按后松手也会触发点击。滑块 / 复选框仍用 `LV_EVENT_VALUE_CHANGED`。

### 4.12 换主题要重建 UI

控件配色是写死在控件上的，改调色板不会影响已创建的对象。`fw_theme_apply()` 切换 LVGL 自带主题的明暗并发布 `SVC_EVENT_THEME_CHANGED` 后，由 `fw_init` 的处理器重建状态栏、浮层以及 App 界面（界面回到初始页），从而立即生效。新增需要跟随主题的 UI 模块时，实现一个 `fw_*_rebuild()` 并在该处理器里登记。

`fw_app_mgr_rebuild_all()` **只立刻重建当前前台 App**，其余已创建的 App 标成 `stale`，等它下次被显示时（`app_launch` / `back` / `back_to_home` 里的 `slot_ensure_created()`）再按新主题重建 —— 早先是全量重建，App 一多切换就要一两秒，而且 `on_create` 里做副作用（天气发请求）的 App 会跟着白做一遍（实测刷出 `esp-tls: Failed to open new connection`）。因此**不要在 `on_create` 里做网络 / 重活**：它可能在后台被重建，也可能因为 `stale` 被延后到显示前才跑；需要"每次进前台都刷新"就写在 `on_start` / `on_resume` 里。

重建必须注意以下四点，否则会崩：

- **不能在 App 事件回调里同步重建**：先把重建 `lv_async_call()` 丢到 LVGL 任务里执行，否则会删掉"正在处理事件的控件"。
- **禁止删除活动屏**：`lv_obj_delete()` 删除当前活动屏时会把 display 的 `act_scr` 置为 NULL，随后任何 `lv_screen_load*()` 都会空指针崩溃。重建前先 `lv_screen_load()` 一块临时空屏，最后确认它已不是活动屏再删除。
- **`fw_window` 的 `s_active` 是裸指针**：旧屏被删除后它悬空，而新屏很可能复用同一地址，导致 `fw_window_switch_to()` 误判"已在目标屏"而跳过切换。切换前先 `fw_window_sync_active()`，且判重时同时核对 `lv_screen_active()`。
- 重建前后台 App 要保持原样：对当前前台 App 依次 `on_destroy` → `on_create` → `on_pause` → `on_start` → `on_resume`，最后再切到它的新根屏。`on_pause` 用来退订 / 停刷新，`on_start` 重新订阅；`on_resume` 不能省 —— 只用 `on_pause` / `on_resume` 管刷新定时器的 App（时钟、音乐、姿态仪、相机、录音机）没有 `on_start`，漏调 `on_resume` 会让它们的定时器一直停在暂停上，界面看着正常但不再刷新。
- **删屏还要提防 `display->prev_scr`**：切屏带了时间（`LV_SCREEN_LOAD_ANIM_FADE_IN` + `FW_APP_ANIM_MS`）时，LVGL 在动画期间把**旧屏**记在 `prev_scr` 上（`lv_display.c` 的 `scr_load_anim_start`），要等动画收尾（`scr_anim_completed`）才清掉。这期间把那块屏删了，下一帧刷新定时器就会走进已释放的内存 —— `lv_display_refr_timer` → `lv_obj_update_layout(prev_scr)` 里的 `lv_obj_get_parent` 报 `LoadProhibited`，寄存器里是个 `0xa5…` 的垃圾指针。所以"马上要删掉的屏"不能是带动画切屏刚离开的那一块：要么改成 `LV_SCREEN_LOAD_ANIM_NONE, 0` 直接切（LVGL 只把 `prev_scr` 清空、不记旧屏），要么等动画结束再删。脚本页就踩过这条：脚本任务离开脚本页时会立刻删掉自己的页，所以 `fw_app_mgr_back_to_home()` 从脚本页回桌面时改成直接切（`fw_app_mgr_back()` 本来就不切屏，由脚本自己 `NONE, 0` 切回去）。

### 4.13 控件必须显式设色，不要依赖 LVGL 自带主题

`lv_theme_default` 会给 `lv_button` 加 `bg_color_primary`（主色底 + **白字**）。我们把按钮底色改成 `bg_card` 之后，按钮里的标签如果自己不设 `text_color`，就会用主题给的白字：深色主题下白字落在深色卡片上看不出来，浅色主题下就是白字白底、**直接消失**。所以：

- `lv_button_create()` 之后，按钮内的标签一律显式 `lv_obj_set_style_text_color()`。
- 滑块（`LV_PART_MAIN` 轨道 / `LV_PART_INDICATOR` 指示条 / `LV_PART_KNOB` 圆点）、输入框、卡片、浮层同理。
- 改完界面跑 `python tools/check_ui_colors.py` 自检。
- 浅色主题下白卡片与浅灰页面靠描边区分，卡片 / 按钮 / 浮层都要 `border_width = 1` + `border_color = fw_theme_color_border()`。

### 4.14 I2C 统一走新版 i2c_master 驱动

Drivers 层不使用旧版 `driver/i2c.h`（ESP-IDF v6 已移除旧驱动）：

- 总线在 `bsp_init()` 里由 `drv_i2c_bus_init()` 建一次（I2C0，GPIO1/2，100 kHz），句柄用 `drv_i2c_bus_handle()` 取。
- 芯片驱动在自己的 `init()` 里 `drv_i2c_device_add(地址, 频率, &s_dev)` 挂设备，之后用 `drv_i2c_read_reg()` / `drv_i2c_write_reg()` 读写寄存器。
- 触摸走 esp_lcd：`esp_lcd_new_panel_io_i2c()` 传入 `drv_i2c_bus_handle()`，并且**必须显式给 `tp_io_config.scl_speed_hz`**（v2 不接受 0）。
- ES8311 / ES7210 音频 codec 由 `drv_es8311.c` / `drv_es7210.c` 自实现寄存器序列，只做 I2C 配置，I2S 数据通路在 `periph_audio`。时钟拓扑固定为 MCLK = 采样率 × 256，该比例下分频系数与采样率无关，所以只有一组系数。

### 4.15 新增 / 删除源文件后必须重新配置

`idf_component_register(SRC_DIRS ...)` 里的目录是 **CMake 配置期**用 glob 展开的，新增或删除 .c/.h 文件后直接 `idf.py build` 不会重新扫描。典型表现是**链接期**报 `undefined reference to xxx`，或反过来报某个 .obj 找不到源文件。

正确做法（改完文件结构先 reconfigure，再 build）：

```powershell
idf.py reconfigure
idf.py build
```

`main/framework/assets/` 这类资源目录增删文件后同理。只改已有文件内容不需要 reconfigure。

### 4.16 看门狗：启动完成后才打开自动重启

Task WDT 由 IDF 启动时初始化（`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`，默认**只告警不重启**）。`svc_watchdog_init()` 在 Services 初始化时把它配成"只告警"，`main.c` 在**内置 Flash 挂载（首次 SPIFFS 格式化）之后**才调 `svc_watchdog_arm()` 打开"喂狗超时自动重启"。不要把这个调用提前：格式化 7 MB 存储分区期间任务长时间不喂狗，提前打开会重启 → 格式化永远做不完 → 启动循环。

纳入监控的任务必须在自己的循环里 `svc_watchdog_subscribe()`（幂等）+ `svc_watchdog_feed()`，且循环周期远小于超时。当前纳入：事件总线派发任务、`svc_power`、`svc_imu`。`svc_watchdog_feed()` 在未纳入的任务里是安全空操作（内部先查 `esp_task_wdt_status`）—— 否则 `esp_task_wdt_reset()` 会每圈打一条 `task not found` 错误日志，20 ms 一圈的任务就能把串口刷爆。**长时间阻塞在队列上的任务不要直接纳入**：`svc_audio`、`ntp_sync_task` 先改成"有限等待 + 喂狗"才安全。

**事件总线任务的栈由订阅回调消费**：所有 `svc_event_bus_subscribe()` 的回调都在 `event_bus_task` 里执行（静态栈 `svc_event_bus.c` 的 `s_task_stack`，现为 6 KB），所以回调里**不要做深调用**——建控件、重填列表这种会一路压到 LVGL 内部（实测 3072 字节时 `App` 的 `refresh_state()` 重填设备列表直接把栈压爆，表现为 `A stack overflow in task event_bus_task`）。正确写法：回调里只置位 / 拷数据，然后 `lvgl_port_lock(超时) + lv_async_call()` 把界面工作交给 LVGL 任务（`fw_init` 的主题重建、`fw_pairing` 的配对浮层、`app_bt` / `app_wifi` 的刷新都这么做）。`lvgl_port_lock()` 在这里给个超时（如 100 ms），拿不到就记一条日志丢掉这次刷新，不要 `0` 硬丢。

### 4.17 崩溃记录：RTC 暂存 + wrap panic handler

`svc_sysinfo` 负责崩溃记录：崩溃时把现场（任务名 / 调用栈 PC / SP / 运行时长）写进 **RTC 不初始化内存**（`RTC_NOINIT_ATTR`），下次启动读出来写 NVS（`sys/crash_log`，只保留最近 3 条）并用 `ESP_LOGE` 打到串口。不用 `espcoredump`：它需要额外的 coredump 分区，而分区表是自定义的。

抓现场靠链接期 wrap IDF 的 panic handler：`main/services/CMakeLists.txt` 的 `-Wl,--wrap=esp_panic_handler` + `svc_sysinfo.c` 的 `__wrap_esp_panic_handler()`。注意三点：只能拦到走 panic handler 的崩溃；**cache 关闭时崩溃**（如 flash 操作）wrap 函数在 flash 里可能来不及写，这种情况启动时只报 `reset=panic (no detail)`；包装函数必须 `IRAM_ATTR` 且不能调 libc（字符串自己搬）；启动时若 `esp_reset_reason()` 是 panic / 看门狗 / brownout，即使没有详细现场也会记一条。

panic 上下文里**不要碰堆**：`heap_caps_get_free_size()` 可能正持着堆锁，会造成二次崩溃（串口表现为 `Panic handler entered multiple times`），所以现场里不记录崩溃瞬间的堆占用；包装函数只做无锁操作 —— RTC 写入、`esp_timer_get_time()`、`pcTaskGetName()`、回溯帧遍历。

「性能监控」App 的任务 CPU 占用依赖 FreeRTOS 运行时统计，`sdkconfig.defaults` 已打开 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`（同时打开 `CONFIG_FREERTOS_USE_TRACE_FACILITY`）。`svc_sysinfo_get_tasks()` 第一次调用只建基准，所有任务的 CPU 百分比都是 0。

验证这条路：临时在代码里调一次 `abort()`，重启后串口应出现 `svc.sysinfo: last crash: reset=panic ... task=... pc=...`，再用 `svc_sysinfo_get_crash_log()` 读 NVS 里的记录。

### 4.18 GCC 14 / 15 与第三方旧 C 代码

工具链是 GCC 15.2（xtensa-esp-elf），GCC 14 起把 `-Wincompatible-pointer-types`、`-Wint-conversion`、`-Wimplicit-function-declaration`、`-Wimplicit-int`、`-Wreturn-mismatch` 这一批从警告默认升级成**错误**。我们依赖的托管组件里就有旧 C 代码会因此挂掉（如 LVGL 的图像解码库把 `uint32_t *` 传给 `unsigned *` 形参）。

`CONFIG_COMPILER_DISABLE_GCC14_WARNINGS` / `CONFIG_COMPILER_DISABLE_GCC15_WARNINGS` 覆盖不到上面这几项，所以真正的处理在顶层 `CMakeLists.txt`：遍历构建组件，**只对托管组件**（名字形如 `namespace__name`）加 `-Wno-error=...`；自己的 `main/drivers|peripherals|services|framework|apps` 不加，保持严格编译。不要去改 `managed_components/` 里的文件 —— 那是构建产物，重新 sync 依赖会被覆盖。

### 4.19 自检脚本（改完代码 / 文案跑一遍）

都不依赖硬件，也不需要构建：

- `python tools/check_cn_text.py` —— 界面文案里的非 ASCII 字符是否能被字体覆盖（ASCII ∪ GBK），以及字体 cmap 是否"含端点"；覆盖不到就改 `tools/gen_fw_fonts.py` 的字符集重新生成。除了 `main/` 下的 .c/.h，它还额外扫一遍 `main/framework/assets/scripts/`（内置示例脚本里的中文也是设备端 LVGL 渲染的）
- `python tools/check_ui_colors.py` —— 创建了标签却没显式设色的地方（浅色主题下会白字白底看不见）
- `python tools/check_api_includes.py` —— 调用了别的层的函数 / 常用 IDF API，但声明它的头文件不可见（GCC 14 / 15 下是错误）
- `python tools/check_decl_order.py` —— 文件内 static 定义晚于使用
- `python tools/check_lvgl_api.py` —— App 层用到的 LVGL 标识符在 `managed_components/lvgl__lvgl` 里是否存在，顺便列出带 `%s` 的 `snprintf` 供人工确认截断风险（GCC 14 / 15 的 `-Werror=format-truncation` 会把 256 字节的 `svc_storage_entry_t.name` 塞进小缓冲直接判错）
- `python tools/check_comments.py` —— 注释里的 Markdown 加粗 / 反引号、`>` 引用块、emoji，以及对外部文档 / 配置文件的引用
- `python tools/check_terms.py` —— 术语写法（`I2C` / `Wi-Fi` / `TF 卡` / `SPIFFS`）
- `python tools/check_deprecated_lvgl.py` —— 用到的 LVGL 已废弃别名（读 `managed_components/lvgl__lvgl` 的 api_map，覆盖 `#define` 与 `typedef` 两类）
- `python tools/check_printf.py` —— 直接使用 `printf` / `fprintf` / `puts`（应统一走 `ESP_LOGx`）
- `python tools/check_tag.py` —— 每个 .c 都有且只有一个 TAG，且前缀与所在层一致
- `python tools/check_app_callbacks.py` —— App 描述符必须有 `on_create` / `name` / `title`；有 `on_pause` 就要有 `on_start` 或 `on_resume`
- `python tools/check_app_registry.py` —— App 描述符定义与 `app_common.c` 的注册一一对应

一键跑全部：`python tools/run_checks.py`（CI 与 pre-commit 钩子用的就是它；只有 `check_*.py` 会被收进去）。单项也能单独跑，退出码 0 = 通过。

这些脚本本身有测试：`python -m unittest discover -s tools/tests`（只需 Python 3 + pillow）。每个自检的坏样例必须报错、好样例必须放行，字体覆盖与 cmap 含端点，图标生成结果必须与仓库里的资源一致（字体那条很慢，`SZPI_TEST_FONTS=1` 才跑）。**改自检脚本或资源生成脚本时，同步 `tools/tests/`。**

### 4.20 内部 RAM 很紧，动配置前先算账

内部 SRAM 只有约 190 KB 可用堆，而下面这些**只能用内部 RAM**的消费者加起来已经把大部分吃掉了：Wi-Fi（静态收发缓冲 + WPA 派生）、BLE（控制器环境池 + 主机任务/队列）、音频 I2S DMA、LVGL 绘制缓冲（6.4 KB，见 4.2）、各服务任务与队列、`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 预留的一块。

在 Wi-Fi 关联 + SPIFFS 挂载那个时间窗里，连小对象都可能分配不出来（`E SPIFFS: mutex lock could not be created` 后 `assert vQueueDelete`，或 `abort() at lock_init_generic`），本质是内部堆空了。

对策（都在 `sdkconfig.defaults`）：

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`：通用 malloc 一律优先 PSRAM（FreeRTOS 对象 / DMA 缓冲走 `MALLOC_CAP_INTERNAL`，不受影响；任务栈默认也走内部，见下一条）
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=16384`：大块 DMA 缓冲都在开机早期分配，运行期只有小请求
- `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`（+ `CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y`）：允许用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` 把栈放 PSRAM。**脚本任务的 8 KB 栈就放 PSRAM**（`fw_script_init()`）—— 它不做 flash 擦写期间的深调用，放外部内存安全；腾出来的内部 DRAM 正好够相机那 7.6 KB 连续块。**别把做 SPIFFS / flash 写入的任务栈放 PSRAM**
- `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8` / `STATIC_TX_BUFFER_NUM=8`：默认 16/16，每块约 1.6 KB 且只能用内部 RAM
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`：mbedTLS 默认从内部 RAM 分配（`MBEDTLS_INTERNAL_MEM_ALLOC`），而 `mbedtls_ssl_setup()` 一次就要十几 KB（握手参数 + 收发缓冲）。Wi-Fi + BLE + LVGL 起来后内部只剩几 KB，HTTPS 请求会在 `esp-tls: mbedtls_ssl_setup returned -0x008D`（= `PSA_ERROR_INSUFFICIENT_MEMORY`）失败 —— 表现是"连上网了但天气 / 下载 / 脚本联网全失败"。改成 PSRAM 分配（`esp_mem.c` 里映射到 `MALLOC_CAP_SPIRAM`）
- LVGL 绘制缓冲 10 行
- `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=8192`：**相机 DVP 的接收缓冲必须是一整块内部连续 DMA**（RGB565 时 `dma_buffer_size = 2 × 半缓冲`，半缓冲由线宽与 `LCD_CAM_DMA_NODE_BUFFER_MAX_SIZE` 算出来）。默认 32768 时 QVGA RGB565 要 30720 字节；降到 8192 后整块是 7680（`node_size` 3840 × 2）。**这块最小就是 7680，改分辨率也降不下来**（node_size 由硬件上限决定）。所以内部 DRAM 必须留得下一块 7680；BLE + Wi-Fi + Web 控制台起来后最大连续块只有 1~3 KB 时就会 `cam_hal: DMA buffer ... malloc failed`。注意 **别开 `esp_camera_set_psram_mode(true)`（PSRAM DMA）**：实测 S3 上相机 DMA 直接写 PSRAM 会把同样在 PSRAM 的 LVGL 堆踩坏（`tlsf_free` / `remove_free_block` 崩在 `StoreProhibited`）。**JPEG 模式另算**：`ll_cam.c` 里写死 `16 × 1024 = 16384` 字节、不受这个配置影响，本板放不下 —— 所以相机 App 与脚本的 `camera.capture()` 都拍 RGB565 再转 BMP（`svc_camera_write_bmp()`，两边共用一套），不切 JPEG。开相机前 `svc_camera_open()` 还会 `svc_web_set_hold(true)` 把 Web 控制台停掉、把它的 6 KB 任务栈还给堆，关相机再 `svc_web_set_hold(false)` 恢复（Wi-Fi 还连着就自动重起）。**音频任务（`svc_audio`）的栈保持 4 KB、不要开大**：任务栈必须在内部 RAM，开大就会挤掉这 7680 的连续块（链接播放的 TLS 握手已改到 6 KB 临时任务里跑）
- App / 脚本里的大静态数组清掉，**static 数组别超过几百字节**

**诊断入口**（一律用 `MALLOC_CAP_DMA` 统计：它正好是"内部 DMA 可用区"，也是 BLE / 音频 / 绘制缓冲 / FreeRTOS 对象真正会耗的那块；`MALLOC_CAP_INTERNAL` 会把 IRAM 算进来，`largest` 会得出比 `free` 还大的怪值）：

- `main.c` 挂载内置 SPIFFS 前：`heap before internal mount: dma-internal free=... largest=...`
- `svc_bt_init()` 开始 / 结束：`init: dma-internal heap free=...` / `init done: ...`
- 串口工具 / 性能监控 App：当前堆余量

### 4.21 新增 App 用到的公共设施（不要各写一套）

写 App 前先看这一节，能省掉大量重复代码，也避免各 App 视觉不一致：

| 用途 | 用谁 |
|------|------|
| 页面根屏 + 内容容器（从状态栏下方开始、内边距 12、行距 8） | `fw_ui_page(&content)` |
| 整行入口（高 44 **卡片内的行**：透明底 + 底部 1 px 分隔线，右侧可显示数值） | `fw_ui_row_btn()`（内置符号）/ `fw_ui_row_btn_img()`（自绘图标，**优先用这个**）+ `fw_ui_row_btn_value()`，放进 `fw_ui_group()` |
| 分组卡片（包住若干设置行，一页 1~2 张，不要让行散在页面上） | `fw_ui_group(parent)`，加完行对本张卡调用 `fw_ui_group_end()` 去掉末行分隔线 |
| 头部动作按钮（高 28，图标 + 可选文字，w>0 固定宽 / 否则撑满） | `fw_ui_icon_btn(parent, &icon_ui_xxx, text, w, cb, user)` |
| 单独摆一个 20×20 图标 | `fw_ui_icon(parent, &icon_ui_xxx, color)` |
| 只读规格 / 数值的九宫格（cols 列 × rows 行，一格 22 px 高） | `fw_ui_table(parent, cols, rows, labels)` + `fw_ui_table_value(table, index, value)`（索引 = 行 × 列数 + 列）；3 列一格约 90 px 只放短值，长组合用 2 列；值可能很长（构建号）时对该格调 `fw_ui_table_value_scroll(table, index, true)` 让它在格内横向滚动 |
| 底部动作条的大按钮（高 44，图标 + 文字居中） | `fw_ui_action_btn(parent, &icon_ui_xxx, text, w, cb, user)`；按钮里图标是第 0 个子对象、文字是第 1 个，播放⇄暂停直接取出来改 |
| 字节数显示（`512 B` / `24 KB` / `7.9 MB` / `1.2 GB`） | `fw_ui_format_size(buf, len, bytes)`，不要各 App 各写一份 |
| "图标 + 标签 + 滑块 + 数值"一行（同样是卡片内的行） | `fw_ui_slider_row(parent, &icon_ui_xxx, label, ...)` + `fw_ui_slider_row_value()` |
| 内容卡片 / 主信息卡（页面主角，2 px 主色描边） | `fw_ui_card(parent, h)` / `fw_ui_hero_card(parent, h)` |
| 设置项分组间隔 | `fw_ui_gap(parent, 10)` |
| 浮层动作按钮 / 浮层输入框 | `fw_ui_btn(parent, text, w, primary, cb, user)` / `fw_ui_textarea(parent, placeholder)` |
| 列表 / 网格 / Toast / 对话框 / 进度条 | `fw_ui_list()`、`fw_ui_list_add()`、`fw_ui_list_add_value()`（右侧数值）、`fw_ui_list_add_icon()`（图标 + 文本 + 数值）、`fw_ui_list_item_value()`（改某一项的数值，不重建行）、`fw_ui_list_hint()`（提示行，无描边）、`fw_ui_list_mark()`（当前项）、`fw_ui_list_value_color()`、`fw_ui_grid()`、`fw_ui_toast()`（同一时刻只保留一个，新的顶掉旧的）、`fw_ui_dialog()`（面板高度随内容，正文不会压到按钮上）、`fw_ui_progress_bar()` + `fw_ui_progress_set()` / `fw_ui_progress_title()`（面板标题就是状态文字，如"已下载 45%"） |
| 一次要建几百项（目录 / 曲库 / 图库） | `fw_ui_stage_create()` + `fw_ui_stage_start()`（每约 16 ms 建一批，界面在构建期间仍可交互），容器删之前 `fw_ui_stage_stop()`、`on_destroy` 里 `fw_ui_stage_destroy()`；不要一次循环建完 |
| 主题色 | 只用 `fw_theme_color_*()`（见 4.13） |
| 系统名 / 版本 / 配网热点名 / 蓝牙广播名 | `main/services/include/svc_identity.h` 的 `SZPI_OS_NAME` / `SZPI_OS_VERSION`（宏）与 `svc_identity_ap_ssid()` / `svc_identity_bt_name()`（函数，实现见 `svc_identity.c`）；两个名字都是 `系统名-地址后 4 位`（大写十六进制：Wi-Fi 用 STA 的 MAC、蓝牙用主机地址），全工程只在那里生成一次 |
| 字体 / 图标 | `fw_asset_font_cn()/cn_large()/14()/20()/24()/32()`；**界面功能图标用 `fw_icons.h` 的 `icon_ui_*`**（20×20 白色 + alpha，运行时 `image_recolor` 染色，由 `tools/gen_fw_icons.py` 生成，App 里不要再画 `LV_SYMBOL_*`）；状态栏状态图标是同一文件里的 `icon_status_*`；桌面彩色图标是 `icon_home_*`（同一个生成脚本） |
| LVGL 显示图片 / GIF（按文件路径） | `fw_asset_fs_path()` 转成 `"A:/sdcard/..."` 再给 `lv_image_set_src()` |
| 目录遍历 | `svc_storage_iter_start/next/end`（`iter_next` 返回的是内部缓冲，用完必须马上拷走） |
| 整块读写小文件（编辑器的文本、相机的 BMP） | `svc_storage_read/write/remove/exists`（读上限 1 MB） |
| 文件管理类操作（删除目录 / 改名 / 复制） | `svc_storage_remove_tree` / `svc_storage_rename`（同存储内瞬间）/ `svc_storage_copy`（分块、支持目录）；都在后台任务里调，别卡 LVGL 任务 |
| 摄像头 | `svc_camera_*`（含 `svc_camera_write_bmp()`：RGB565 帧 → 24 位 BMP，App 与脚本都用它，别自己再写一套 BMP 封装；不要把 `esp_camera.h` 引进 App） |
| 系统信息 / 最近日志 / 任务 CPU | `svc_sysinfo_get()`、`svc_sysinfo_get_recent_logs()`、`svc_sysinfo_get_tasks()` |
| 蓝牙中心角色（连外设 / 读写特征） | `svc_bt_central_*`（连接 / 发现服务 / 读 / 写 / 订阅；回调在 BTC 任务里，界面要 `lv_async_call` 转回 LVGL 任务） |
| 外扩 GPIO / PWM / I2C / UART / ADC / CAN | `svc_io_*`（不要把 `driver/gpio` 等引进 App） |
| 时间 / 时区 / 12 / 24 小时制 | `svc_time_now()` / `svc_time_format()`、`svc_time_set_timezone()` / `svc_time_get_timezone()`（参数是 `svc_time_tz_t`：POSIX TZ 串 + 城市显示名，一起存 `sys/timezone`）、`svc_time_set_manual()`、`svc_time_get_24h()` / `svc_time_set_24h()`（存 `sys/clock_24h`，状态栏与时钟 App 共用，改状态栏时间格式也走它） |

两条硬约束：**App 不直接调 IDF / Peripherals / Drivers**（缺接口就往 Services 加薄封装）；**不要在 App 里加大块 static 缓冲**（内部 RAM 只有十几 KB 余量，见 4.20）。

内容类 App（`Music` / `Image` / `Editor`）统一支持 `App?path=<绝对路径>`，从文件管理点文件直接打开：音频直接播、图片直接看、文本 / 脚本直接编辑。**读参数放 `on_create` 与 `on_resume` 两处** —— 从返回栈回来那条路径只发 `on_resume`，首次创建时 `on_resume` 又不发，只放 `on_start` 会漏（`on_start` 留着恢复刷新定时器）。普通 `fw_app_mgr_launch()` 会清空参数，所以不带参数进前台不会误触发。

页面的整体风格（单屏、头部行、主信息卡、详情格、无数据占位、浮层）照 `docs/03-design/02-ui-system.md` **12.6 节**来 —— 那是时钟 / 日历 / 天气三个 App 沉淀下来的套路，别自创一套。

### 4.22 脚本系统的边界

- 运行时、脚本管理、能力绑定都在 `fw_script`；脚本硬件能力一律经 `svc_io`，**不直接调 Peripherals / Drivers**。
- 同一时刻只运行一个前台脚本；脚本退出 / 异常时由 `fw_script` 统一回收它创建的界面对象、定时器、订阅。
- 脚本执行在 `script_task`（优先级 4，核心 0），不要在脚本绑定里做长阻塞操作。
- 脚本异常不得影响系统：错误（行号 + 描述）写日志并提示，不允许把异常抛到 C 层。错误统一经 `script_error()` 收口：写 `fw_script_last_error()`（成功启动 / 正常停止后清空）、发 `SVC_EVENT_SCRIPT_FAILED`；同一段错误重复出现时只提示一次（坏掉的定时器每秒报一次不会刷屏），回调出错**不**停脚本。
- 脚本界面由脚本自己建，`fw_script` 负责切屏与回收；**脚本总有自己的一页**：无界面脚本由 `fw_script` 给一个「运行日志」页显示它的 `print` 输出（`print` 被换成自己的实现：一行进脚本日志环 + 串口日志，标签 `script`）。界面能力里 `ui.canvas(parent, w, h)` 是一块脚本自己画像素的 RGB565 位图（`fill` / `rect` / `line` / `circle`，颜色写 `0xRRGGBB`，**宽必须偶数**否则 LVGL 会内部拷一份、改了像素不显示），缓冲区随页面回收。**离开脚本页就停脚本**：返回键 / BOOT 单击 / BOOT 双击 / 主页键分别在 `fw_app_mgr_back()`、`fw_app_mgr_back_to_home()` 里判断 `fw_script_owns_screen()`（BOOT 单击走的也是 `fw_app_mgr_back()`），换主题的 `fw_app_mgr_rebuild_all()` 也会先停（脚本停止会把屏还给启动它的 App），脚本页不会留在后台回不去。脚本也可以自己调 `sys.exit()` 结束：它用特殊错误值立刻展开当前 Lua 调用栈（`script_pcall()` 认得这个值，不当成错误），收尾和用户点「停止」一致。
- 脚本不放固定目录：`fw_script_scan()` 递归扫 TF 卡与内置存储整盘，收录所有 `.lua`（扩展名不分大小写），`/sdcard/scripts` 只是内置示例的落地目录。整盘递归有几十到上百毫秒，调用方**不要放在 LVGL 任务里**（脚本管理 App 用一次性任务扫，扫完再回 LVGL 任务换列表）。
- 脚本的**网页能力**（`web.get` / `web.post`）挂在设备 Web 控制台的 `/s` 前缀下，用 `svc_web_set_custom_handler()` 接进去（`/s` 与 `/s/*` 两个 handler 在 `svc_web` 里最后注册，见 4.32）。请求在 HTTP 任务里进来、Lua 只在脚本任务里跑，中间是"HTTP 任务拷请求 → 等应答信号量（最多 2 s）→ 脚本任务调 Lua 回调写响应"的桥：**回调要尽快返回**（它慢多久整个控制台就等多久，超过 500 ms 打一条告警），别在回调里做 `net.http_get` 这类阻塞操作；响应体上限 24 KB（PSRAM 缓冲按需分配）。脚本停止时 `web_routes_reset()` 清掉路由，此刻若还有请求挂在那儿就直接回 503，不让 HTTP 任务干等到超时。
- **内置示例脚本统一放真实文件里**：`main/framework/assets/scripts/` 下的 `counter.lua` / `clock.lua` / `snake.lua` / `io.lua` / `gomoku.lua` / `api-reference.txt`，`main/framework/CMakeLists.txt` 用 `EMBED_FILES` 嵌进固件（符号名按 IDF 规则**只取文件名**：`counter.lua` → `_binary_counter_lua_start`），`release_samples()` 只列"卡上的名字 + 资源起止"。**不要再把示例写成 C 字面量**：几十上百行转义字符串既难改也容易错（引号、`\n`、中文混在一起）。两个细节：资源文件名必须 ASCII —— C 符号名由文件名生成，中文会变成一串下划线，所以「脚本接口参考.txt」的资源文件叫 `api-reference.txt`、释放到卡上才叫中文名；行尾也照原样（.lua 用 LF、这份 .txt 用 CRLF，Windows 记事本打开不乱）。
- **包含 Lua 头文件的组件必须自己定义 `LUA_32BITS=1`**：`espressif/lua` 只把 `-DLUA_32BITS=1` 加在**自己组件的 PRIVATE** 编译选项上（`lua_Integer` / `lua_Number` 是 32 位），而它的 `port/include/luaconf.h`「头文件注入」对别人无效 —— `lua.h` 用的是引号 `#include "luaconf.h"`，编译器先在同目录找到 `lua/luaconf.h`（没有 `LUA_32BITS`）。两边数值类型不一致时，`luaL_newlib()` 里的 `luaL_checkversion()` 会报 `core and library have incompatible numeric types`，而且它跑在不受保护的上下文里，**直接 abort 重启**（表现为"一运行脚本就重启"）。`main/framework/CMakeLists.txt` 用 `target_compile_definitions(${COMPONENT_LIB} PUBLIC LUA_32BITS=1)` 对齐。注意 32 位上限：`lua_Integer` 是 `int`（2^31-1），往 Lua 里推的值别超过它（epoch 秒、堆字节数都还在范围内）。
- 脚本下载在「下载」App 里做（`.lua` 落到脚本目录），脚本管理只做列出 / 运行 / 编辑 / 删除，不要重复实现下载。
- 脚本绑定接口（Lua 侧命名）见 `docs/03-design/03-app-framework.md`，改动时同步该文档。绑定目前覆盖：界面（含输入框 / 对话框 / 列表 / 按钮 / 画布 / 通用样式方法）、输入、事件、定时器（`every` / `after` / `cancel`）、硬件 IO（含 I2C 寄存器读写）、文件（含 `append` / `size`）、音频、HTTP（GET / POST + 请求头）、MQTT、WebSocket、BLE（从机 / HID / 中心角色）、摄像头、IMU、设置、`util`（base64 / hex）、`json`、`web`（局域网网页路由）。**系统级设置（亮度 / 主题 / 时区）不开放**：脚本只能读写自己的 `settings` 命名空间与文件，不动 NVS 里的全局配置。

### 4.23 往日志里挂钩子必须做重入保护

`svc_sysinfo` 为了给「系统日志」App 提供"最近日志"，用 `esp_log_set_vprintf()` 装了钩子：把每行抄进无锁环，再调用原来的输出函数。这个钩子必须自己防重入，因为**日志输出路径内部会再打日志**：`uart_write_bytes()` 开头的 `ESP_RETURN_ON_FALSE` 一旦条件不满足就 `ESP_LOGE`，这条日志又走输出 → 回到钩子 → 再写 UART → 再失败……**无限递归**，实测表现是满屏重复回溯 + `assert xQueueSemaphoreTake`，而且因为 panic 里又打日志，重启循环。

做法（见 `svc_sysinfo.c`）：用"当前任务是否已在本函数里"判断重入（`xTaskGetCurrentTaskHandle()` 与保存的 owner 比对，重入直接 `return 0`），行缓冲放**静态区**而不是调用者栈上，并且**剩余栈不足时不抄录**（只保留原有串口输出）—— `vsnprintf` 的栈开销算在调用者头上（几百字节到 ~1 KB，本工程用 `-Og`），BTC_TASK（3072 B，被 Bluedroid 日志吃到临界）实测会被压爆栈。**代价是这类栈很紧的任务的日志只会出现在串口、不进日志环**，对不上属正常，别为此去掉这个保护；跳过时每 64 行会在串口打一条 `log ring: <任务名> 剩余栈不足…`，一眼看出是谁。

这条判断**必须用"当前"剩余栈，不能用 `uxTaskGetStackHighWaterMark()`**：那是"历史最低水位"，任务只要在初始化阶段深压过一次（几乎每个 IDF / Bluedroid 任务都会），水位就永远偏低，之后它所有日志都会被丢掉 —— 表现是「系统日志」里只剩少数几个"一直很浅"的任务的行、很久才刷新一次、跟串口差很多。现在的做法是 `xTaskGetStackStart(NULL)`（栈底最低地址）+ 当前栈指针求差（栈向下生长，差就是还没用到的部分），阈值 `SYSINFO_LOG_MIN_STACK`（1024）。

**钩子要尽早装**：`svc_sysinfo_log_capture_start()` 放在 `main.c` 的 `app_main()` 第一句（`svc_sysinfo_init()` 里再调一次兜底）。装在 Services 初始化里太晚 —— bsp_init / peripherals_init / 前面几个服务的日志全都进不了环，「系统日志」App 的内容会从中间某一行开始，跟串口对不上。环大小 2 KB（BSS）、单行最多 256 字节，超出部分截断（截断行会补一个换行，免得和下一条粘在一起）。

「系统日志」App 的位置与之配套：日志环存的是"最后 N 字节"，开头可能截在半行上，显示前先跳到第一个换行；**跟随时要在 `lv_label_set_text()` 之后先 `lv_obj_update_layout()` 再 `lv_obj_scroll_to_y(box, LV_COORD_MAX)`** —— LVGL 的滚动范围是按子对象当前坐标算的，不先布局就只能滚到上一次的底部，表现是"总差最后几行、不像跟最新"。另外只有在用户本来就贴着底部时才跟随（`lv_obj_get_scroll_bottom(box) <= 2`），翻上去看历史时不要被新日志顶走。刷新时内容与上一帧完全一致就直接跳过，别每 3 s 白重排一次大标签。

### 4.24 BOOT 键用轮询 + 稳定性去抖，不要改回边沿中断

`drv_key` 每 10 ms 采一次 GPIO0，电平连续 30 ms 不变才认可一次按下 / 松开。早期版本用 `GPIO_INTR_ANYEDGE` + 松手时计时：触点抖动会被误判成"第二次按下"，**单击会丢失、双击要按得很重才认**，而且长按只能等松手才判定。现在长按（1.5 s）**在按住期间**上报，`fw_input` 立刻弹电源菜单；交互映射固定为单击 → 返回上一级、双击 → 回桌面、长按 → 电源菜单。

熄屏时按键与触摸都**只负责唤醒**：不导航、不落到界面、也不发事件给脚本。`fw_input` 的唤醒分支在发布 `SVC_EVENT_KEY` 之前就返回；触摸那边由 `periph_touch` 丢掉整次手势（`svc_power` 也不转发 `SVC_EVENT_TOUCH`）。两条路要保持一致，别只改一边。

### 4.25 图片资源的 RGB565 字节序必须是小端

`fw_logo.c`（开机 Logo）与 `fw_icons.c`（三组图标）里的 RGB565 平面按**小端**存放，因为 `periph_lcd_flush_cb()` 会在送屏前把整块缓冲再交换一次。资源若按大端生成，屏幕上会得到 R/B 互换的错色（开机 Logo 曾如此）。用 `tools/gen_fw_icons.py` 重新生成图标时不要手工改字节序。

### 4.26 I2S 改时钟只能在主机通道做，且通道必须先 disable

`i2s_channel_reconfig_std_clock()` / `i2s_channel_reconfig_tdm_clock()` 要求通道处于 disable 状态，否则直接返回 `ESP_ERR_INVALID_STATE`（IDF 日志：`I2S should be disabled before reconfiguring the clock`）。全双工下 IDF 会把 RX 切成从机、跟着同一条总线的 BCLK/WS 走，所以录音侧**不要**单独 reconfig RX 的时钟 —— 只改主机（TX）的时钟 + 对应 codec 的采样率寄存器即可（见 `periph_audio_set_format()`）。踩这个坑的表现是：录音机按下去没反应、状态栏麦克风图标不亮（录音根本没起来）。

**录音必须把"读 I2S"和"写 FATFS"分开**：RX 的 DMA 环只有几十毫秒（实测一次最多读出 3840 字节 ≈ 60 ms），而写卡在跨簇时 FATFS 要更新 FAT / 目录项、会卡上百毫秒。两者放在同一个循环里必然二选一 —— 实测把读拖停后 RX 溢出、`i2s_channel_read()` 一直失败，录音每 1~3 s 丢 0.2~0.4 s（更早的表现是"录 1 秒自己停掉"，`record_wav()` 的循环 `break` 出去）。现在的做法（`svc_audio.c`）：

- `audio_task` 只读 I2S，往 PSRAM 里的环形缓冲（64 KB ≈ 1 s）塞；塞不下就丢这一块、**绝不阻塞**；
- `rec_write_task`（栈 4 KB）从缓冲取数据写文件，卡顿都卡在它这里；结束时由它回填 WAV 头并关文件（文件归它独占）；
- **写卡任务必须挂到"另一个核心"**（`AUDIO_REC_WRITE_CORE`）：SDMMC / FATFS 写卡时会长时间占住所在核心，跟读侧同核心会把读侧饿死 —— 实测同核心时 RX 每 ~250 ms 溢出一次、每次只收到几百字节（比"读写放一个循环里"还糟），换核心后写卡再慢也只影响它自己；
- **读到的数据一块都不能丢**：`i2s_channel_read()` 返回超时 / 溢出时，`bytes_read` 里可能已经有几千字节真音频 —— 按 `bytes_read` 收下来，**只有 `bytes_read == 0` 才算"真卡住"**、才 `periph_audio_rx_restart()` + 重试（连续 5 次 200 ms 才认输）。把带错误的整块丢掉过，听感就是每几百毫秒一个"滴"的断点；
- **定期 `fsync`**（每写满 64 KB）：FATFS 只在关文件时才把长度写进目录项，录音过程中去看文件一直是 0 字节，看起来像"没落盘"。注意 `FILE*` 上**没有** `f_sync()`（那是 FatFs 私有 API，要 `FIL*`），`fflush()` 在 `vfs_fat` 里也没接任何东西 —— 正确的是 `fsync(fileno(fp))`，`vfs_fat_fsync()` 里做的就是 `f_sync(file)`；
- 单生产者单消费者，`head` / `tail` 各自只由一方写，两个任务在不同核心上，所以配 `__atomic_thread_fence`（release / acquire），不用锁 —— 读侧绝不能等锁。

加大 RX 环不可行（要一块内部连续 DMA 内存，最大块只有 15 KB，见 4.20）；环形缓冲放 PSRAM，不占那块。收尾日志 `record done: N bytes read, N written, N dropped, N partial` 一眼能看出还有没有溢出（`partial` 是"报错但拿到了数据"的次数，`dropped` 是缓冲塞满丢掉的）。

### 4.27 开机画面之前不能出现任何界面元素

背光在 `periph_lcd_init()` 末尾就点亮了，而 Logo 要等 `fw_boot_animation()` 才上屏，中间约 0.6 s。这段必须只显示黑屏：`fw_init()` 在建状态栏**之前**把 `lv_layer_top()` 藏起来（状态栏、Toast、对话框都挂在这一层），由 `fw_boot_animation()` 收尾时再打开；`periph_lcd_init()` 把默认屏底色设成**纯黑**（不要用页面底色 `bg_primary`）。少做任何一件，开机时就会先露出状态栏或深灰底，看起来像"先闪一下桌面"。新增开屏前的初始化代码时，不要往 `lv_layer_top()` 或活动屏上放可见控件。

### 4.28 `lv_obj_is_valid()` 不能用来判断悬空指针

LVGL 9.6 里 `lv_obj_is_valid()` 只是 `lv_obj_is_in_widget_tree()` 的别名（`include/lvgl/api_map/lv_api_map_v9_5.h` 里的宏），它会顺着 `obj->parent` 一路往上找。所以**对已经释放的对象解引用会直接崩**（`LoadProhibited`，寄存器里能看到一个像 `0x39` 这种垃圾指针），而且因为它是宏，编译器不会提醒。踩过的表现：状态栏 Toast 记着上一个 Toast 的指针，`if (lv_obj_is_valid(s_toast))` 一执行就崩在 `lv_obj_is_in_widget_tree`。

正确做法是自己管生命周期：

- 保存对象句柄时，在对象的 `LV_EVENT_DELETE` 回调里把句柄清成 NULL 再收尾（`fw_ui_dialog` 的 `s_dialog`、`fw_ui_toast` 的 `s_toast` 都这么做）；删除是异步的，回调里还要按对象比对，避免旧对象的延迟删除误清新句柄。
- 挂在对象上的 `lv_timer` 也要在同一个回调里删掉：定时器的 user_data 存着对象指针，定时器活过对象照样崩。
- 批量创建的对象（脚本界面）要在自己的注册表里判断"还是不是我建的那些"，而不是问 LVGL。

### 4.29 在 on_pause 里停了刷新的 App，on_start 与 on_resume 都要恢复

`fw_app_mgr` 进入前台有两条路径，回调不一样：

| 进入方式 | 回调 |
|----------|------|
| 首次创建（`on_create` 之后） | `on_start` |
| 重新进入，App 还在返回栈里（`back()` 回来） | `on_resume` |
| 重新进入，但用主页键回过桌面（`back_to_home()` 会清栈） | `on_start`（App 早就在，不会再 `on_create`） |

所以在 `on_pause` 里 `lv_timer_pause()` 的 App，必须把恢复逻辑**同时挂到 `on_start` 和 `on_resume`**（时钟 App 就是一个函数挂两处）。只挂 `on_resume` 的典型症状：第一次进 App 秒数正常跳，用主页键回桌面再点进来就永远停在那一秒 —— 界面看着完全正常，只是不再刷新。

框架侧也做了兜底：`app_launch()` 发现"App 已创建、只是不在返回栈里"时，`on_start` 之后会补一次 `on_resume`。两边都写，是为了以后改框架时不再踩。

要在"离开 App"时停止本地动作（播放 / 录音 / 计时）的，用 `fw_app_mgr_is_foreground("App 名")` 区分"真的走了"和"换主题原地重建"：`on_pause` 时框架已经把前台标记清掉，返回假说明用户真离开了。「音乐」「录音机」「秒表」「计时器」都这么做 —— 界面回到后台还接着走秒 / 放音是最容易漏掉的一类问题（换主题是前台原地重建，标记不动，不打断）。

### 4.30 App 里的异步 HTTP 请求

`svc_http_get_async()` 每次调用都新建一个临时任务（栈 6 KB，只能用内部 RAM），回调在 `svc.http` 任务里执行。写这类 App 注意：

- 回调里碰 LVGL 必须 `lvgl_port_lock()`；**请求类型通过 `user` 传进去**，别用共享的静态变量（两个请求一起飞时会错位）。
- 响应缓冲是调用方的内存：**同时只发一个请求**（一个 busy 标志），否则两个请求会往同一个缓冲里写；也顺带省一个 6 KB 栈的任务。
- 缓冲**不要随 App 一起释放**：请求在飞的时候 App 可能因为换主题被 `on_destroy` + `on_create`，释放就是 use-after-free（任务还在往里写）。天气 App 的做法是缓冲首次分配后常驻。
- URL 有长度上限（`SVC_HTTP_URL_MAX`，现在 512）：Open-Meteo 的天气请求渲染出来 367 字节，超了会被 `ESP_ERR_INVALID_SIZE` 拒掉（表现是"请求失败"，城市搜索却正常，因为那条只有 92 字节）。
- 回调触发时 UI 指针可能已经是 NULL（App 销毁过）：每个写 UI 的地方都要判空。

### 4.31 Wi-Fi 只在需要时 start，扫描 / 连接前要确保已 start

`svc_net` 的 STA 不是开机就启动：只有「有保存凭据的自动重连」「SmartConfig」「AP 配网」三条路径会 `esp_wifi_start()`。所以：

- `svc_net_wifi_scan()` / `svc_net_wifi_connect()` 自己要补一次 start（没有保存凭据时开机不会 start）。否则扫描直接返回 `ESP_ERR_INVALID_STATE`（界面只看到空列表、没有任何日志），连接只会拿到 `NOT_STARTED`，而"等 `STA_START` 再补连"的 pending 永远等不到 —— 表现就是"扫描不到 / 连不上"。
- AP 配网收尾 `svc_net_prov_stop()` 里的 `stop → STA → start` 会把刚建立的连接断掉（`s_connect_pending` 在拿到 IP 时已清零），必须按保存的凭据重新发起一次连接，否则"配网提示成功却一直连不上"。
- 配网期间的 `SVC_EVENT_WIFI_CONNECTED` 要按"网页里提交的目标 SSID"过滤（`s_prov_target`）：STA 驱动里可能还留着旧网络配置，切 APSTA 的 `esp_wifi_start()` 会让它自动连回旧 AP。若把这种事件当成配网成功，热点会在一两秒内被 `prov_stop()` 关掉 —— 表现就是"点了配网热点，电脑 / 手机根本搜不到"（日志里 `provisioning started` 之后紧跟 `connected to AP` 再 `provisioning stopped`）。
- `svc_net_wifi_forget()` 除了清 NVS，还要 `esp_wifi_set_config(WIFI_IF_STA, &空配置)`：驱动里（`WIFI_STORAGE_RAM`）的旧配置不清掉的话，下次 `esp_wifi_start()` 仍会拿它自动连回旧网络（"忘记网络之后又自己连上了"）。
- SmartConfig 的事件回调**只注册一次**（放在 `svc_net_init()`）：在 `svc_net_smartconfig_start()` 里注册会在反复配网时累积回调，手机重复广播时同一个事件被处理多次，反复 `esp_wifi_set_config()` 把刚建立的连接打断（日志表现：重复 `smartconfig got ssid`、`smartconfig already stopped`、`E wifi:sta is connecting, cannot set config`、连上几秒后又 `disconnected reason=8`）。配网进行中的判断放在回调里（`if (!s_smartconfig) return;`）。
- **STA 连上就停掉 SmartConfig**：`WIFI_EVENT_STA_CONNECTED` 里如果 `s_smartconfig` 还开着就 `svc_net_smartconfig_stop()`。不停的话 SC 嗅探器一边想设信道、一边又因为 "STA is scanning or connecting" 设不了，串口会被 `W: wifi:STA is scanning or connecting... cannot set channel` + `I smartconfig: smartconfig errno -1@sc_sniffer.c 156` 刷屏几十秒（保存的凭据自己连上时就会这样）。

### 4.32 局域网 Web 管理页（svc_web）

设备连上 Wi-Fi 后在 80 端口提供网页（系统状态 + 文件管理），断开自动停；页面与接口见 `docs/03-design/01-services.md` 第 15 节。写这块时容易踩的几条：

- **80 端口和配网页是同一个端口**：STA 刚连上时配网 httpd 可能还没停（`prov_stop()` 稍后才跑），`httpd_start()` 会失败 —— 起服务要退避重试（每 500 ms，最多 12 次），别一次失败就放弃。起停本身放在 `svc.web` 任务里做，事件总线回调里只投一条命令（回调跑在 `bus_task` 的 6 KB 静态栈上，别在那里建服务）。
- **页面用 `EMBED_FILES` 编进固件**（`main/services/CMakeLists.txt` + `assets/web/index.html`）：不依赖文件系统、也不会被用户误删。**符号名只取文件名**（IDF 的 `data_file_embed_asm.cmake` 里 `get_filename_component(... NAME)` 之后 `MAKE_C_IDENTIFIER`），所以 `assets/web/index.html` 对应 `_binary_index_html_start` / `_end`，写成带目录的名字会在链接期 `undefined reference`。
- **上传 / 下载必须流式**：`svc_storage_read/write` 是整块的（读有 1 MB 上限），大文件要走 `svc_storage_write_open/write_chunk/write_close` 与 `svc_storage_stream_read`。写卡那条路径里非内部 RAM 的来源会先过内部 DMA 缓冲（见 4.7），别绕过 `svc_storage` 自己 `fopen/fwrite`。
- **请求体按行流式解析**：一次删除可能带几百个路径，别 `malloc` 一大块整读（内部 RAM 紧），按 1 KB 读缓冲 + 单行缓冲边收边处理；单行超长时**整行作废**，否则会拿半截路径去删文件。
- **路径必须校验落在 `/sdcard` / `/internal` 里**：页面不做认证，别让它碰到别的挂载点；粘贴还要拦住"把目录贴进它自己内部"（`svc_storage_copy` 会无限递归）。
- **内置存储（SPIFFS）不能建目录**：IDF 的 SPIFFS VFS `mkdir` 直接返回 `ENOTSUP`，所以 `/api/mkdir` 对 `/internal/*` 要回明确错误，页面与文件管理 App 都要把「新建文件夹」禁掉（`svc_storage_mkdir` 在 TF 卡才有效）。
- **配网期间先别起 Web 服务**：配网页占着 80 端口，而且那时报的"已连接"可能是旧 AP，`httpd_start()` 会一直 `error in listen (112)`。`svc_web` 收到 `WIFI_CONNECTED` 时先问 `svc_net_prov_is_active()`，配网中直接跳过，等配网结束重连那次再起。
- **`esp_http_server` 只有一个任务**：handler 里不要碰 LVGL，也不要长时间阻塞 —— 一个请求不返回，后面所有请求（含状态轮询）都在排队。**耗时操作（复制 / 剪切 / 删除）不要放在 handler 里**：提交给 `svc.web` 任务做，handler 立刻返回 `{"started":true}`，界面轮询 `GET /api/job` 显示进度（`s_job` 是单例，同时只允许一个任务）。handler 里几百字节的局部缓冲可以接受（栈设 6144）；更大的拼串 / 转义缓冲**别做成内部 RAM 的 static 数组**（加起来十几 KB，会把脚本任务的栈挤掉）—— 统一走服务起来时分配的 PSRAM 共享暂存 `s_scratch`，所有 handler 都在同一个任务里顺序执行，共用是安全的。
- **`lru_purge_enable` 要关掉**：开着的话，大文件上下传期间排队中的状态轮询会被 LRU 清理踢掉，浏览器那边就是"连接失败"，页面上闪一下"连接断开"。关掉之后排队的请求只是等，不会报错（浏览器一个 host 也就 2~6 条连接，不会把 7 个 socket 占满）。页面侧配合做防堆积：上一次轮询没回来就跳过这一次，上传期间干脆不发轮询。
- **通配路由要开 `uri_match_fn`**：`httpd_config_t.uri_match_fn` 默认是精确比较，`"/s/*"` 这种模板匹配不上；设成 `httpd_uri_match_wildcard` 后模板才生效，而模板里没有 `*` / `?` 的精确路径不受影响。匹配按**注册顺序**走，所以精确接口先注册、`/s`（脚本区）最后注册，互相不抢。`HTTP_ANY` 可以让一个 handler 接所有方法（脚本区的 GET / POST 就共用一个 `h_script`）。

---

## 五、存储与分区预算（不要超）

| 资源 | 容量 | 用途 |
|------|------|------|
| Flash | 16 MB | 全部固件 + 资源 |
| PSRAM | 8 MB Octal @ 80 MHz | 图片 / 音频 / 摄像头大 buffer、LVGL 对象内存 |

分区布局（`partitions.csv`）：`nvs` 24 KB、`phy_init` 4 KB、`factory` 8 MB、`storage`（SPIFFS）7 MB。**不做 OTA，所以没有 ota 分区**。完整表见 `docs/02-architecture/00-overview.md` 第 8 节。

- 添加大资源前先让人类执行 `idf.py size-components` 检查。
- 仓库位于 `D:\workspace\szpi-esp32s3`（Windows 路径，无空格；`.vscode` 配置写死）。

---

## 六、构建 / 烧录工作流（命令由人类执行）

**AI 不要自行运行以下任何命令**，仅供文档记录与人类复制：

```powershell
# 1. 进入 IDF 环境（PowerShell）
C:\esp\v6.1\esp-idf\export.ps1

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
- 调试：`.vscode/launch.json` 是 `gdbtarget` attach 配置，OpenOCD + `board/esp32s3-bridge.cfg`。
- LSP：clangd 读取 `build/compile_commands.json`。让人类先跑一次 `idf.py build`。

**ESP-IDF v6.1 迁移要点**（影响依赖与配置）：

- 旧版 I2C 驱动已移除，统一用新版 `i2c_master`（本项目本来就用新版）。
- Wi-Fi 配网组件由 `wifi_provisioning` 改为 `network_provisioning`。
- JSON、MQTT 已拆成独立组件（不再随 IDF 内置）。

---

## 七、启动序列

```
1. svc_sysinfo_log_capture_start() → 装日志钩子（最早做，日志环才能装下整个开机过程）
2. nvs_flash_init()
3. bsp_init()                 → I2C, LEDC, PCA9557, LCD, Touch, Key, IMU
4. peripherals_init_all()     → IO, Audio, LCD(+LVGL display), Touch, IMU, Storage, Button, Ext
                                 └─ periph_lcd_init() 内初始化 LVGL 并注册显示
                                    periph_touch_init() 注册 LVGL input device
5. services_init()            → Watchdog, EventBus, Settings, Storage, BT, Time, Audio, Net,
                                 Power, IMU, IO, Camera, SysInfo, Web（连上 Wi-Fi 后才真正起服务）
6. fw_init()                  → Theme, Asset, Window, AppMgr, Script, UI, StatusBar, Pairing, Input
7. app_register_all()         → 注册所有内置 App
8. fw_boot_animation()        → 全屏官方 Logo 静态展示 + 单声开机提示音（约 2.05 s）
9. fw_app_mgr_launch("Home")  → 显示桌面
10. periph_storage_mount(内置) → 首屏之后挂载内置 SPIFFS（首次自动格式化）
11. svc_watchdog_arm()        → 启动完成，打开看门狗超时自动重启
```

时间预算（目标：上电到桌面 < 5 s；含开机画面 2.05 s）见 `docs/03-design/04-data-flow.md`。

---

## 八、典型数据流

### 触摸事件

drv_ft6336 → periph_touch 扫描任务（缓存 + 点击 / 长按识别）→ LVGL indev read_cb 读缓存 → LVGL input device 派发 → 当前界面处理

### 音频播放

App / 脚本 调用 `svc_audio_play(&src)` → `svc_audio` 任务 → VFS 读文件 → helix MP3 解码（或 WAV PCM）→ `periph_audio_write()` → I2S0 DMA → ES8311 → NS4150B 功放 → 喇叭（PA_EN 由 PCA9557.BIT1 控制）

### 脚本运行

脚本管理 App 选脚本 → `fw_script_run(path)` → `script_task` 加载 Lua → 脚本经绑定调用界面 / Services / `svc_io` → 停止或异常退出 → 释放资源、发布 `SVC_EVENT_SCRIPT_STOPPED`

### 配置持久化

App / 脚本 调用 `svc_settings_set(key, value)` → NVS write → 触发 `SVC_EVENT_*_CHANGED` → 订阅者刷新（App 在 `on_start` 订阅、`on_pause` 退订）

### IMU 运动 / 姿态

`svc_imu` 任务（50 ms 轮询；QMI8658 中断引脚未引出）→ 判定姿态变化 / 摇晃 / 抬手 → 发布 `SVC_EVENT_IMU_MOTION` / `SVC_EVENT_IMU_ORIENTATION` / `SVC_EVENT_IMU_SHAKE` / `SVC_EVENT_IMU_PICKUP` → `svc_power` 熄屏时由**抬手**唤醒（只订阅 `SVC_EVENT_IMU_PICKUP`；判据是"平放静止 ≥ 1 s 后屏幕立起来（约 30° 以上）并保持 200 ms"，摇晃 / 运动不唤醒，避免碰到就亮屏）

详细见 `docs/03-design/04-data-flow.md`。

---

## 九、代码与文档的优先级（冲突时）

1. `main/main.c` + `sdkconfig` + `partitions.csv` + `dependencies.lock`（真正会参与构建的）
2. `docs/02-architecture/*.md`（目标架构）
3. `docs/01-requirements/*.md`（意图）
4. `docs/03-design/*.md`（详细设计）

(1) 与 (2) 冲突时，按 (1) 修正 (2)。注意 `00-original-requirement.md` / `01-hardware-spec.md` 是最原始文档（见 1.1）：只按实现修正事实性矛盾，不借它们扩写范围。

---

## 十、本仓库**没有**的东西（不要去找）

- 没有固件端单元测试：既没有 Unity 组件测试，也没有 pytest-embedded；固件行为只在真机上验证。
- 没有代码覆盖率配置。
- 没有 `CLAUDE.md` / `.cursor/rules`（`AGENTS.md` 是唯一的 agent 指引）。

CI、工具测试、pre-commit 与容器见第十一节。如需新增上面这些，**先与人类确认**再提议。

---

## 十一、自动化（CI / 测试 / 格式 / 容器）

- **CI**：`.github/workflows/ci.yml`。push 到 `main` / `ci/*`、对 `main` 的 PR、手动触发时跑：
  `checks`（`python tools/run_checks.py` + 工具单元测试）、`build`（自检通过后才跑；在
  `espressif/idf:v6.1` 容器里编译，再用 `idf.py merge-bin` 合并成单一固件 `releases/szpi-esp32s3.bin`，
  上传该固件与 `.elf` / `.map`），以及只在 main 推送时跑的 `refresh`（把合并固件提交回仓库，让仓库里的
  固件始终是最新一次 main 构建；提交带 `[skip ci]`）。ccache 用 `actions/cache` 缓存 `~/.ccache`。
- **一键发布**：`.github/workflows/release.yml`，手动触发填版本号提升方式（patch / minor / major）
  或直接给版本号。流程是改版本号 → 编译合并 → 提交版本号与 `releases/szpi-esp32s3.bin` → 打标签 →
  建 GitHub Release（固件作为资产上传）。
- **版本号**：全工程只在 `main/services/include/svc_identity.h` 的 `SZPI_OS_VERSION` 定义一次，
  由 `tools/bump_version.py` 读写（保留分段数与前导 `v`，只改那一行）。
- **单一固件**：`releases/szpi-esp32s3.bin` 是 bootloader + 分区表 + 应用的合并镜像，串口烧录工具
  选它、地址填 `0x0`。它是构建产物但随版本提交（发布时直接当资产用），不要手改。
- **工具测试**：`tools/tests/`（Python `unittest`）。测的是**工具脚本**而不是固件：每个 `check_*.py`
  的坏样例 / 好样例、字体覆盖与 cmap 含端点、图标生成的字节序与"生成结果与仓库一致"。
  `python -m unittest discover -s tools/tests`，只需 Python 3 + pillow，不依赖硬件与 ESP-IDF。
- **静态自检**：`python tools/run_checks.py` 一次跑完所有 `check_*.py`（清单见 4.19）。
- **pre-commit**：`.pre-commit-config.yaml`。提交前跑静态自检，推送前跑工具测试，对改动的
  C / 头文件跑 clang-format（`pip install pre-commit` 后 `pre-commit install`）。
- **clang-format**：`.clang-format`（4 空格缩进、函数大括号另起一行、控制语句大括号同行、指针靠左、
  列宽 100；包含顺序、注释换行、字符串换行都不动）。仓库此前没有统一格式，首次对全树执行会有
  一处格式 diff，单独提交一次。
- **容器**：`docker/Dockerfile`（构建镜像，带 ccache 与 pillow / pre-commit）与 `.devcontainer/`
  （VS Code 开发容器），都基于 `espressif/idf:v6.1`，用法见 `docker/README.md`。

改动这些文件时保持同步：新增自检脚本要加测试；只有 `check_*.py` 会被 `run_checks.py` 收进去，别的
工具脚本要显式接进 CI 或 pre-commit 钩子。
