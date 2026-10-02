# AGENTS.md — szpi-esp32s3 (SZPI-OS)

## 一、语言与开发约定

- **语言约定**：本项目所有文档（`README.md`、`AGENTS.md`、`docs/`、代码注释）默认使用简体中文，与 `docs/` 现有内容保持一致。
- **文本与术语约定**：更新任一层文档或代码注释时，逐篇核对以下写法（`00-original-requirement.md`、`01-hardware-spec.md` 已冻结，是这些写法的基准）：
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
| `docs/01-requirements/00-original-requirement.md`、`01-hardware-spec.md` | **冻结**，作为最原始文档，任何情况下都不改 |
| `docs/01-requirements/02-prd.md` | **受控**：仅当人类要求、或代码变更必须同步时才改 |
| `docs/02-architecture/`、`docs/03-design/`、`AGENTS.md`、`README.md` | 随代码实现随时可更新 |

结构性的东西（分区表、关键配置项）放在可更新的架构文档（`docs/02-architecture/00-overview.md`），不要写进冻结的硬件规格。

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
│   ├── framework/          # Framework 层（含 assets/ 字体子集与开机 Logo）
│   └── apps/               # Apps 层（含 src/app_register.c 注册表）
├── managed_components/     # 组件管理器自动填充，按构建产物对待
├── tools/                  # 自检脚本与字体生成
├── docs/                   # 设计文档（需求 / 架构 / 详细设计）
└── build/                  # 构建产物（在 .gitignore）
```

**关键文件说明**：

- `main/main.c` —— 启动入口：NVS → `bsp_init()`（Drivers：I2C0 GPIO1/2 100 kHz → LEDC 背光 GPIO42 → PCA9557 @ 0x19 → ST7789 屏 SPI3_HOST 40/41/39 80 MHz 模式 2 → FT6336 单点触摸 @ 0x38 → BOOT 键 GPIO0 → QMI8658 @ 0x6A）→ `peripherals_init_all()` → `services_init()` → `fw_init()` → `app_register_all()` → `fw_boot_animation()` → `fw_app_mgr_launch("Home")` → 挂载内置 SPIFFS → `svc_watchdog_arm()`。
- `main/idf_component.yml` —— 组件依赖（LVGL 9、esp_lvgl_port 2.x、esp_lcd_touch_ft5x06、esp32-camera、helix MP3、esp_websocket_client、espressif/lua 等）。ES8311 / ES7210 音频 codec 不用组件，见 `main/drivers/src/drv_es8311.c`、`drv_es7210.c` 自实现的寄存器驱动。
- `dependencies.lock` —— 精确锁定版本，**禁止手改**。升级时改 `main/idf_component.yml` 或 `sdkconfig.defaults`，让构建工具重新生成。
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
| `main/peripherals/` | `periph_lcd`、`periph_touch`、`periph_audio`、`periph_imu`、`periph_storage`、`periph_camera`、`periph_io_exp`、`periph_button`、`periph_ext`（外扩 GPIO / PWM / I2C / UART / ADC） |
| `main/services/` | `svc_event_bus`、`svc_settings`、`svc_storage`、`svc_time`、`svc_audio`、`svc_net`、`svc_bt`、`svc_mqtt`、`svc_ws`、`svc_power`、`svc_imu`、`svc_io`、`svc_camera`、`svc_sysinfo`、`svc_watchdog` |
| `main/framework/` | `fw_app_mgr`（原生 App）、`fw_script`（Lua 运行时 + 脚本管理 + 绑定）、`fw_window`、`fw_input`、`fw_theme`、`fw_asset`、`fw_ui`、`fw_statusbar`、`fw_boot_animation` |
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

全局浮层挂在 `lv_layer_top()` 上时，必须先 `lv_obj_clear_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE)`：`lv_obj` 默认带 `LV_OBJ_FLAG_CLICKABLE`，否则浮层会吞掉全屏触摸（LVGL 命中顺序为 layer_sys → layer_top → 当前屏）。

### 4.2 LVGL 绘制缓冲放内部 DMA 内存，不放 PSRAM

`lvgl_port_display_cfg_t.flags.buff_dma = true` + `buff_spiram = false`（10 行 = 320×10×2 = 6.4 KB，单缓冲）。

原因：ESP32-S3 的 `esp_ptr_dma_capable()` 只覆盖内部 DRAM（`SOC_DMA_LOW~SOC_DMA_HIGH` = 0x3FC88000~0x3FD00000），**PSRAM 不在其中**，所以绘制缓冲放 PSRAM 时 SPI 驱动会为**每一笔 flush** 临时 `heap_caps_aligned_alloc(..., MALLOC_CAP_DMA)` 一块同样大小的内部回弹缓冲（本板 `max_transfer_sz` 是整屏，一次 flush 就是 10 KB 级）。Wi-Fi / BLE 起来后这笔分配会失败 → `panel_io_spi_tx_color: spi transmit (queue) color failed` → 刷屏失败；而 `esp_lvgl_port` 自带的 flush 回调忽略返回值、也不调 `lv_display_flush_ready()`，单缓冲下 LVGL 会死等这块缓冲 —— 表现是 LVGL 任务占满 CPU、界面永久卡住、事件总线任务被饿死触发看门狗。`periph_lcd_flush_cb()` 覆盖了它的 flush 回调，失败时补一次 `lv_display_flush_ready()`（最多丢一帧，绝不死锁）并打印错误码。

### 4.3 LVGL 9 与 v8 的差异

本项目用 **LVGL 9**（`LV_COLOR_16_SWAP` 已移除、`lv_disp_*`→`lv_display_*`、`lv_img_*`→`lv_image_*`、`lv_btn_*`→`lv_button_*`、`LV_MEM_CUSTOM`→`LV_USE_STDLIB_MALLOC`）。几条常被踩的：

- **RGB565 字节交换**：LVGL 9 不再有 `LV_COLOR_16_SWAP`，在显示 flush 回调里做（`LV_COLOR_FORMAT_RGB565_SWAPPED` 或 `lv_draw_sw_rgb565_swap()`）。
- **内存分配**：`sdkconfig.defaults` 里设 `CONFIG_LV_USE_CLIB_MALLOC=y`（choice 里选中 CLIB，LVGL 的 `LV_USE_STDLIB_MALLOC` 随之等于 `LV_STDLIB_CLIB`，去走 IDF 堆，受 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 影响）。注意 `CONFIG_LV_STDLIB_CLIB` 只是个 `int` 常量、**不是**开关，写成 `=y` 会被 Kconfig 静默忽略而回落到内置 TLSF 分配器。不要再用 `LV_MEM_CUSTOM`。
- **图片解码**：PNG 是 `LV_USE_LODEPNG`、JPEG 是 `LV_USE_TJPGD`（旧版叫 `LV_USE_PNG`）。
- **LVGL 9.6 废弃了通用 flag 接口**：`lv_obj_add_flag` / `lv_obj_clear_flag` / `lv_obj_remove_flag` / `lv_obj_has_flag` / `lv_obj_set_flag` 都改成专用 setter，如 `lv_obj_set_scrollable(o, false)`、`lv_obj_set_clickable(o, true)`、`lv_obj_set_hidden(o, true)`、`lv_obj_is_hidden(o)`。用户自定义 flag（`LV_OBJ_FLAG_USER_1`）没有专用 setter，改用 `lv_obj_set_user_data()` / `lv_obj_get_user_data()` 传标记（对象由 `lv_malloc_zeroed` 分配，`user_data` 初值必为 NULL）。`lv_timer_t` 已是不可见类型，读它的 data 要用 `lv_timer_get_user_data(t)`，不能写 `t->user_data`。
- **Lua 的 `lualib.h` 预声明了标准库入口符号**（`luaopen_io` / `luaopen_os` / `luaopen_string` / `luaopen_math` 等）：`fw_script.c` 里脚本模块的 C 入口不能与它们同名，否则「static declaration follows non-static declaration」。我们的 `io` 模块入口因此叫 `luaopen_io_hw`。
- **工具链实际是 GCC 15.2**（`esp-15.2.0_20251204`，`-std=gnu23`），4.18 那批降级清单在它上面依然适用。
- 上述选项在 ESP-IDF 下经 lvgl 组件的 Kconfig 配置。

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

BLE HID 设备模拟（PRD `NET-009`）也在 `svc_bt` 内：`svc_bt_hid_dev.c/h`（GATT 服务本体与属性表）、`svc_bt_hid_send.c/h`（报告发送）、`svc_bt_hid_report.c/h`（报告构造与用法值），符号统一 `svc_bt_hid_*` / `SVC_BT_HID_*`，TAG 统一 `svc.bt.hid`。**Bluedroid 只允许一个 GATTS 回调**（GAP 同理），所以回调统一由 `svc_bt` 注册，事件再按 app_id / gatts_if 转发给 HID；将来新增 GATT 服务必须走这条"一个回调 + 分发"的路，否则后注册的会覆盖先注册的。HID 报告要等配对（加密）完成才能发。

两个**初始化时序 / 参数**坑（都曾表现为"状态停在 `state -> 1`、手机搜不到设备"）：

- HID 事件的转发开关必须**在注册 app 之前**置位。`esp_ble_gatts_app_register()` 是异步的，BTC 任务优先级又高于调用者，REG 事件常常在函数返回前就回调进来；若此时开关还没打开，事件被丢弃，服务就不会创建。
- 广播数据里的 **`service_uuid_len` 必须是 16 的整数倍**：`esp_ble_gap_config_adv_data()` 里有 `if (service_uuid_len & 0xf) return ESP_ERR_INVALID_ARG;`，直接传 2 字节的 16 位 UUID 会被拒收。要广播 16 位服务 UUID（如 HID `0x1812`）必须用 16 字节的 128 位形式（Bluetooth base UUID，`[12]/[13]` 就是那两个字节）。
- 广播 / 扫描**不依赖 GAP 完成事件**：把 `esp_ble_gap_config_adv_data()` / `esp_ble_gap_set_scan_params()` 返回 `ESP_OK` 当作就绪，顺序由 BTA / BTU 单队列保证；`ADV_START_COMPLETE` 到达时仍用于确认，失败则回退并按 3 s / 最多 5 次重试。
- **连接 / 断开事件会按"每个注册过的 GATT app"各回调一次**，所以 `gatts_cb` 里的连接 / 断开处理必须按 `gatts_if` 过滤，`svc_bt_note_conn()` 要幂等。

排查入口：启动日志里的 `callbacks: gatts=... gap=... match=x/y`（用 `esp_ble_gatts_get_callback()` / `esp_ble_gap_get_callback()` 读真正生效的回调指针）、`selftest: state=... adv(ready/active) scan_ready=...`（`svc_bt_init()` 后 2 s 自动打印）。

### 4.7 FATFS 三项必须同时保留

`storage` 分区上 `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_CODEPAGE_936=y`、`CONFIG_FATFS_API_ENCODING_UTF_8=y` 三项必须一起保留，否则 TF 卡上的非 ASCII 长文件名会乱码。

### 4.8 中文界面文案与字体子集

中文由 `main/framework/assets/font_cn14.c` / `font_cn16.c` 渲染（Noto Sans SC 栅格化），是**只含界面用字的子集**。新增中文文案若出现方框，说明用到了字表外的字 —— 把该字加进 `tools/cn_chars.py` 的 `CN_CHARS` 并重新生成（源字体在 `tools/fonts/NotoSansSC-VF.ttf`，路径可省略）：

```powershell
python tools/gen_cn_font.py --sizes 14,16
```

字体统一从 `fw_asset_font_cn()`（14 px 正文）/ `fw_asset_font_cn_large()`（16 px 标题）/ `fw_asset_font_14|20|24()`（拉丁）获取，不要直接引用字体变量。开机画面用 `main/framework/assets/image_lckfb_logo.c`。

**字体生成按 LVGL 9 的字体格式**。

动态数据（如 Wi-Fi 名称、文件名）里的汉字不在字表内是正常的 —— UI 字体回退到 `font_cn_extra`（`--cs gb2312` 生成，GB2312 一级 3755 常用字，14 px / 2bpp），两条生成命令：

```powershell
python tools/gen_cn_font.py --cs gb2312 --sizes 14 --bpp 2
python tools/gen_cn_font.py --sizes 14,16
```

回退链：`font_cn14`/`font_cn16` → `font_cn_extra` → `lv_font_montserrat_14`（FontAwesome 符号）。

**字体 cmap 的 `range_length` 必须含端点**：LVGL 查表用的是 `rcp < range_length`（`lv_font_fmt_txt.c`），生成脚本里的 `range_length` 要写 `last - first + 1`。写成 `last - first` 时**码点最大的那个字永远查不到字形**，配合 `CONFIG_LV_USE_FONT_PLACEHOLDER=y` 就显示成方框（踩过的例子：字表里码点最大的是 `？` U+FF1F，日历"设为今天？"对话框里的问号就是方框）。`tools/check_cn_text.py` 会检查这一条，报错就重新跑 `python tools/gen_cn_font.py --sizes 14,16`。

### 4.9 点亮背光前必须先清屏

ST7789 的 GRAM 掉电 / 复位后不会自动清空。若先开背光再等 LVGL 首帧，会短暂显示**上一次运行残留在面板里的画面**（表现为开机"先闪一下主页"）。`periph_lcd_init()` 在设置背光前先 `periph_lcd_fill(0x0000)` 整屏清黑。

复位与 CS 时序**不要动**：本板面板 RST 是 NC，官方例程的顺序是"先 `esp_lcd_panel_reset()`（此时 CS 仍为高）→ 再拉低 CS → 再 `esp_lcd_panel_init()`"。曾把"拉低 CS"挪到复位之前，SWRESET 就真正生效了，而 ST7789 复位后需要约 120 ms 才能接受新命令（IDF 内部只等 20 ms），随后的初始化命令被丢弃——表现是**开机只有背光、没有画面**。

### 4.10 开机提示音要先打成功放

`periph_audio_set_mute(false)` 之后功放 / codec 有几百毫秒的启动斜坡。若紧接着播放很短的提示音，开头会被这段斜坡吃掉（听起来"没有声音"）。`fw_boot_animation()` 先解除静音、等 150 ms，再播放 300 ms 的提示音。

### 4.11 点击请用 LV_EVENT_SHORT_CLICKED

LVGL 的 `indev_proc_release` 会**无条件**发送 `LV_EVENT_CLICKED`，只有 `LV_EVENT_SHORT_CLICKED` 才判断"无长按、无滑动"。所以界面上的"点击"处理要注册 `LV_EVENT_SHORT_CLICKED`，否则长按后松手也会触发点击。滑块 / 复选框仍用 `LV_EVENT_VALUE_CHANGED`。

### 4.12 换主题要重建 UI

控件配色是写死在控件上的，改调色板不会影响已创建的对象。`fw_theme_apply()` 切换 LVGL 自带主题的明暗并发布 `SVC_EVENT_THEME_CHANGED` 后，由 `fw_init` 的处理器重建状态栏、浮层以及所有 App / 脚本的界面（界面回到初始页），从而立即生效。新增需要跟随主题的 UI 模块时，实现一个 `fw_*_rebuild()` 并在该处理器里登记。

重建必须注意以下四点，否则会崩：

- **不能在 App 事件回调里同步重建**：先把重建 `lv_async_call()` 丢到 LVGL 任务里执行，否则会删掉"正在处理事件的控件"。
- **禁止删除活动屏**：`lv_obj_del()` 删除当前活动屏时会把 display 的 `act_scr` 置为 NULL，随后任何 `lv_screen_load*()` 都会空指针崩溃。重建前先 `lv_screen_load()` 一块临时空屏，最后确认它已不是活动屏再删除。
- **`fw_window` 的 `s_active` 是裸指针**：旧屏被删除后它悬空，而新屏很可能复用同一地址，导致 `fw_window_switch_to()` 误判"已在目标屏"而跳过切换。切换前先 `fw_window_sync_active()`，且判重时同时核对 `lv_screen_active()`。
- 重建前后台 App 要保持原样：对当前前台 App 依次 `on_destroy` → `on_create` → `on_pause` → `on_start` → `on_resume`，最后再切到它的新根屏。`on_pause` 用来退订 / 停刷新，`on_start` 重新订阅；`on_resume` 不能省 —— 只用 `on_pause` / `on_resume` 管刷新定时器的 App（时钟、音乐、姿态仪、相机、录音机）没有 `on_start`，漏调 `on_resume` 会让它们的定时器一直停在暂停上，界面看着正常但不再刷新。

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

Task WDT 由 IDF 启动时初始化（`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`，默认**只告警不重启**）。`svc_watchdog_init()` 在 Services 初始化时把它配成"只告警"，`main.c` 在**内置 Flash 挂载（首次 SPIFFS 格式化）之后**才调 `svc_watchdog_arm()` 打开"喂狗超时自动重启"（PRD `SYS-003`）。不要把这个调用提前：格式化 7 MB 存储分区期间任务长时间不喂狗，提前打开会重启 → 格式化永远做不完 → 启动循环。

纳入监控的任务必须在自己的循环里 `svc_watchdog_subscribe()`（幂等）+ `svc_watchdog_feed()`，且循环周期远小于超时。当前纳入：事件总线派发任务、`svc_power`、`svc_imu`。`svc_watchdog_feed()` 在未纳入的任务里是安全空操作（内部先查 `esp_task_wdt_status`）—— 否则 `esp_task_wdt_reset()` 会每圈打一条 `task not found` 错误日志，20 ms 一圈的任务就能把串口刷爆。**长时间阻塞在队列上的任务不要直接纳入**：`svc_audio`、`ntp_sync_task` 先改成"有限等待 + 喂狗"才安全。

### 4.17 崩溃记录：RTC 暂存 + wrap panic handler

`svc_sysinfo` 负责 PRD `SYS-004`：崩溃时把现场（任务名 / 调用栈 PC / SP / 运行时长）写进 **RTC 不初始化内存**（`RTC_NOINIT_ATTR`），下次启动读出来写 NVS（`sys/crash_log`，只保留最近 3 条）并用 `ESP_LOGE` 打到串口。不用 `espcoredump`：它需要额外的 coredump 分区，而分区表是自定义的。

抓现场靠链接期 wrap IDF 的 panic handler：`main/services/CMakeLists.txt` 的 `-Wl,--wrap=esp_panic_handler` + `svc_sysinfo.c` 的 `__wrap_esp_panic_handler()`。注意三点：只能拦到走 panic handler 的崩溃；**cache 关闭时崩溃**（如 flash 操作）wrap 函数在 flash 里可能来不及写，这种情况启动时只报 `reset=panic (no detail)`；包装函数必须 `IRAM_ATTR` 且不能调 libc（字符串自己搬）；启动时若 `esp_reset_reason()` 是 panic / 看门狗 / brownout，即使没有详细现场也会记一条。

panic 上下文里**不要碰堆**：`heap_caps_get_free_size()` 可能正持着堆锁，会造成二次崩溃（串口表现为 `Panic handler entered multiple times`），所以现场里不记录崩溃瞬间的堆占用；包装函数只做无锁操作 —— RTC 写入、`esp_timer_get_time()`、`pcTaskGetName()`、回溯帧遍历。

「性能监控」App 的任务 CPU 占用依赖 FreeRTOS 运行时统计，`sdkconfig.defaults` 已打开 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`（同时打开 `CONFIG_FREERTOS_USE_TRACE_FACILITY`）。`svc_sysinfo_get_tasks()` 第一次调用只建基准，所有任务的 CPU 百分比都是 0。

验证这条路：临时在代码里调一次 `abort()`，重启后串口应出现 `svc.sysinfo: last crash: reset=panic ... task=... pc=...`，再用 `svc_sysinfo_get_crash_log()` 读 NVS 里的记录。

### 4.18 GCC 14 与第三方旧 C 代码

工具链是 GCC 14（xtensa-esp-elf），它把 `-Wincompatible-pointer-types`、`-Wint-conversion`、`-Wimplicit-function-declaration`、`-Wimplicit-int`、`-Wreturn-mismatch` 这一批从警告默认升级成**错误**。我们依赖的托管组件里就有旧 C 代码会因此挂掉（如 LVGL 的图像解码库把 `uint32_t *` 传给 `unsigned *` 形参）。

`CONFIG_COMPILER_DISABLE_GCC14_WARNINGS` 覆盖不到上面这几项，所以真正的处理在顶层 `CMakeLists.txt`：遍历构建组件，**只对托管组件**（名字形如 `namespace__name`）加 `-Wno-error=...`；自己的 `main/drivers|peripherals|services|framework|apps` 不加，保持严格编译。不要去改 `managed_components/` 里的文件 —— 那是构建产物，重新 sync 依赖会被覆盖。

### 4.19 五个自检脚本（改完代码 / 文案跑一遍）

都不依赖硬件，也不需要构建：

- `python tools/check_cn_text.py` —— 界面文案用到的汉字是否都在字表里（缺字就加进 `tools/cn_chars.py`，再 `python tools/gen_cn_font.py --sizes 14,16` 重新生成字体）
- `python tools/check_ui_colors.py` —— 创建了标签却没显式设色的地方（浅色主题下会白字白底看不见）
- `python tools/check_api_includes.py` —— 调用了别的层的函数，但声明它的头文件不可见（GCC 14 下是错误）
- `python tools/check_decl_order.py` —— 文件内 static 定义晚于使用
- `python tools/check_lvgl_api.py` —— App 层用到的 LVGL 标识符在 `managed_components/lvgl__lvgl` 里是否存在，顺便列出带 `%s` 的 `snprintf` 供人工确认截断风险（GCC 14 的 `-Werror=format-truncation` 会把 256 字节的 `svc_storage_entry_t.name` 塞进小缓冲直接判错）

### 4.20 内部 RAM 很紧，动配置前先算账

内部 SRAM 只有约 190 KB 可用堆，而下面这些**只能用内部 RAM**的消费者加起来已经把大部分吃掉了：Wi-Fi（静态收发缓冲 + WPA 派生）、BLE（控制器环境池 + 主机任务/队列）、音频 I2S DMA、LVGL 绘制缓冲（6.4 KB，见 4.2）、各服务任务与队列、`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 预留的一块。

在 Wi-Fi 关联 + SPIFFS 挂载那个时间窗里，连小对象都可能分配不出来（`E SPIFFS: mutex lock could not be created` 后 `assert vQueueDelete`，或 `abort() at lock_init_generic`），本质是内部堆空了。

对策（都在 `sdkconfig.defaults`）：

- `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`：通用 malloc 一律优先 PSRAM（FreeRTOS 对象 / 任务栈 / DMA 缓冲走 `MALLOC_CAP_INTERNAL`，不受影响）
- `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=16384`：大块 DMA 缓冲都在开机早期分配，运行期只有小请求
- `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8` / `STATIC_TX_BUFFER_NUM=8`：默认 16/16，每块约 1.6 KB 且只能用内部 RAM
- LVGL 绘制缓冲 10 行
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
| 整行入口（高 50、卡片底 + 描边，右侧可显示数值） | `fw_ui_row_btn()` + `fw_ui_row_btn_value()` |
| "标签 + 滑块"一行 | `fw_ui_slider_row()` |
| 列表 / 网格 / Toast / 对话框 / 进度条 | `fw_ui_list()`、`fw_ui_list_add()`、`fw_ui_grid()`、`fw_ui_toast()`（同一时刻只保留一个，新的顶掉旧的）、`fw_ui_dialog()`、`fw_ui_progress_bar()` |
| 主题色 | 只用 `fw_theme_color_*()`（见 4.13） |
| 字体 / 图标 | `fw_asset_font_cn()/cn_large()/14()/20()/24()`；状态栏图标是 `fw_status_icons.h` 里的 `icon_status_*`（20×20 白色 + alpha，运行时 `image_recolor` 染色，6 个图标的绑定语义见 `docs/03-design/02-ui-system.md` 第 2 节）；桌面彩色图标是 `fw_home_icons.h` 里的 `icon_home_*`；卡片内的功能性图标用 LVGL `LV_SYMBOL_*` / `fw_asset_symbol_for(app_name)`（两个图标生成脚本见 `tools/`） |
| LVGL 显示图片 / GIF（按文件路径） | `fw_asset_fs_path()` 转成 `"A:/sdcard/..."` 再给 `lv_image_set_src()` |
| 目录遍历 | `svc_storage_iter_start/next/end`（`iter_next` 返回的是内部缓冲，用完必须马上拷走） |
| 整块读写小文件（编辑器的文本、相机的 BMP） | `svc_storage_read/write/remove/exists`（读上限 1 MB） |
| 摄像头 | `svc_camera_*`（不要把 `esp_camera.h` 引进 App） |
| 系统信息 / 最近日志 / 任务 CPU | `svc_sysinfo_get()`、`svc_sysinfo_get_recent_logs()`、`svc_sysinfo_get_tasks()` |
| 外扩 GPIO / PWM / I2C / UART / ADC | `svc_io_*`（不要把 `driver/gpio` 等引进 App） |
| 时间 / 时区 / 12-24 小时制 | `svc_time_now()` / `svc_time_format()`、`svc_time_set_timezone()` / `svc_time_get_timezone()`（POSIX TZ 字符串，存 `sys/timezone`）、`svc_time_set_manual()`、`svc_time_get_24h()` / `svc_time_set_24h()`（存 `sys/clock_24h`，状态栏与时钟 App 共用，改状态栏时间格式也走它） |

两条硬约束：**App 不直接调 IDF / Peripherals / Drivers**（缺接口就往 Services 加薄封装）；**不要在 App 里加大块 static 缓冲**（内部 RAM 只有十几 KB 余量，见 4.20）。

### 4.22 脚本系统的边界

- 运行时、脚本管理、能力绑定都在 `fw_script`；脚本硬件能力一律经 `svc_io`，**不直接调 Peripherals / Drivers**。
- 同一时刻只运行一个前台脚本；脚本退出 / 异常时由 `fw_script` 统一回收它创建的界面对象、定时器、订阅。
- 脚本执行在 `script_task`（优先级 4，核心 0），不要在脚本绑定里做长阻塞操作。
- 脚本异常不得影响系统：错误（行号 + 描述）写日志并提示，不允许把异常抛到 C 层。
- 脚本绑定接口（Lua 侧命名）见 `docs/03-design/03-app-framework.md`，改动时同步该文档。

### 4.23 往日志里挂钩子必须做重入保护

`svc_sysinfo` 为了给「系统日志」App 提供"最近日志"，用 `esp_log_set_vprintf()` 装了钩子：把每行抄进无锁环，再调用原来的输出函数。这个钩子必须自己防重入，因为**日志输出路径内部会再打日志**：`uart_write_bytes()` 开头的 `ESP_RETURN_ON_FALSE` 一旦条件不满足就 `ESP_LOGE`，这条日志又走输出 → 回到钩子 → 再写 UART → 再失败……**无限递归**，实测表现是满屏重复回溯 + `assert xQueueSemaphoreTake`，而且因为 panic 里又打日志，重启循环。

做法（见 `svc_sysinfo.c`）：用"当前任务是否已在本函数里"判断重入（`xTaskGetCurrentTaskHandle()` 与保存的 owner 比对，重入直接 `return 0`），行缓冲放**静态区**而不是调用者栈上，并且**剩余栈不足时不抄录**（`uxTaskGetStackHighWaterMark(NULL) < 512` 就跳过）—— `vsnprintf` 的栈开销算在调用者头上，BTC_TASK（3072 B，被 Bluedroid 日志吃到临界）实测会被压爆栈。

界面侧配套的坑：**不要在定时刷新的页面上放超大自动换行标签**：超大的 `LV_LABEL_LONG_WRAP` 标签会让 LVGL 任务长时间卡在字体布局与 glyph 查询上，把同核的 `svc_imu` 饿死 → 看门狗 abort。这类页面只显示最近 512 B、刷新间隔 3 s、不自动滚动。

### 4.24 BOOT 键用轮询 + 稳定性去抖，不要改回边沿中断

`drv_key` 每 10 ms 采一次 GPIO0，电平连续 30 ms 不变才认可一次按下 / 松开。早期版本用 `GPIO_INTR_ANYEDGE` + 松手时计时：触点抖动会被误判成"第二次按下"，**单击会丢失、双击要按得很重才认**，而且长按只能等松手才判定。现在长按（1.5 s）**在按住期间**上报，`fw_input` 立刻弹电源菜单；交互映射固定为单击 → 返回上一级、双击 → 回桌面、长按 → 电源菜单（PRD IN-003）。

### 4.25 图片资源的 RGB565 字节序必须是小端

`image_lckfb_logo.c`、`icons_home.c` 与 `icons_status.c` 里的 RGB565 平面按**小端**存放，因为 `periph_lcd_flush_cb()` 会在送屏前把整块缓冲再交换一次。资源若按大端生成，屏幕上会得到 R/B 互换的错色（开机 Logo 曾如此）。用 `tools/gen_home_icons.py` / `tools/gen_status_icons.py` 这类脚本重新生成时不要手工改字节序。

### 4.26 I2S 改时钟只能在主机通道做，且通道必须先 disable

`i2s_channel_reconfig_std_clock()` / `i2s_channel_reconfig_tdm_clock()` 要求通道处于 disable 状态，否则直接返回 `ESP_ERR_INVALID_STATE`（IDF 日志：`I2S should be disabled before reconfiguring the clock`）。全双工下 IDF 会把 RX 切成从机、跟着同一条总线的 BCLK/WS 走，所以录音侧**不要**单独 reconfig RX 的时钟 —— 只改主机（TX）的时钟 + 对应 codec 的采样率寄存器即可（见 `periph_audio_set_format()`）。踩这个坑的表现是：录音机按下去没反应、状态栏麦克风图标不亮（录音根本没起来）。

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
1. nvs_flash_init()
2. bsp_init()                 → I2C, LEDC, PCA9557, LCD, Touch, Key, IMU
3. peripherals_init_all()     → IO, Audio, LCD(+LVGL display), Touch, IMU, Storage, Button, Ext
                                 └─ periph_lcd_init() 内初始化 LVGL 并注册显示
                                    periph_touch_init() 注册 LVGL input device
4. services_init()            → Watchdog, EventBus, Settings, Storage, BT, Time, Audio, Net,
                                 Power, IMU, IO, Camera, SysInfo
5. fw_init()                  → Theme, Asset, Window, AppMgr, Script, UI, StatusBar, Input
6. app_register_all()         → 注册所有内置 App
7. fw_boot_animation()        → 全屏官方 Logo 静态展示 + 单声开机提示音（约 2.05 s）
8. fw_app_mgr_launch("Home")  → 显示桌面
9. periph_storage_mount(内置) → 首屏之后挂载内置 SPIFFS（首次自动格式化）
10. svc_watchdog_arm()        → 启动完成，打开看门狗超时自动重启
```

时间预算（目标：上电到桌面 < 5 s；含开机画面 2.05 s）见 `docs/03-design/04-data-flow.md`。

---

## 八、典型数据流

### 触摸事件

drv_ft6336 → periph_touch 扫描任务（缓存 + 点击 / 长按识别）→ LVGL indev read_cb 读缓存 → LVGL input device 派发 → 当前界面处理

### 音频播放

App / 脚本 调用 `svc_audio_play(&src)` → svc_audio 的 play_task → VFS 读文件 → helix MP3 解码（或 WAV PCM）→ `periph_audio_write()` → I2S0 DMA → ES8311 → NS4150B 功放 → 喇叭（PA_EN 由 PCA9557.BIT1 控制）

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

(1) 与 (2) 冲突时，按 (1) 修正 (2)。注意 `00-original-requirement.md` / `01-hardware-spec.md` 已冻结（见 1.1），不能靠改它们来消解冲突。

---

## 十、本仓库**没有**的东西（不要去找）

- 没有 CI 工作流（无 `.github/`、无 `.gitlab-ci.yml`）。
- 没有主机端单元测试，没有 Unity / pytest-embedded 框架，没有代码覆盖率配置。
- 没有 pre-commit 钩子，没有 `clang-format`、没有 linter 配置。
- 没有 Docker / devcontainer。
- 没有 `CLAUDE.md` / `.cursor/rules`（`AGENTS.md` 是唯一的 agent 指引）。

如需新增其中任何一项，**先与人类确认**再提议。
