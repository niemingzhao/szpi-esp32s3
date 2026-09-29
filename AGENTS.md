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
| `drivers/` | `pca9557`、`st7789`、`ft6336`、`qmi8658`、`es8311`、`es7210`、`gc0308`、`i2c`、BOOT 按键、LEDC、BSP | 已实现（TF 卡的 sdmmc/fatfs 挂载由 Peripherals 层的 `periph_storage` 直接用 IDF 组件完成，不单独设驱动） |
| `peripherals/` | `periph_lcd_*`、`periph_touch_*`、`periph_audio_*`、`periph_imu_*`、`periph_storage_*`、`periph_io_exp_*`、`periph_button_*`、`periph_camera_*` | 已实现：LCD/Touch/Button/IMU/Storage/IO/Audio（播放 + ES7210 录音）/Camera（按需初始化，不参与启动） |
| `services/` | `svc_event_bus`、`svc_settings`、`svc_storage`、`svc_time`、`svc_audio`、`svc_net`、`svc_bt`、`svc_bt_hid`、`svc_power`、`svc_imu`、`svc_watchdog`、`svc_notification`、`svc_ota`、`svc_mqtt`、`svc_ws`、`svc_sysinfo`、`svc_shell`、`svc_camera` | 全部已实现：事件总线 / 设置（含 blob、恢复出厂设置）/ 存储（含 TF 热插拔、目录迭代、整块读写、格式化）/ 时间 / 音频（WAV + MP3 + tone 播放、ES7210 录音）/ 网络（STA + HTTP + SmartConfig + AP/Web 配网）/ 蓝牙 BLE（GATT 从机 + 广播 + 扫描 + HID 设备模拟）/ 电源（背光、熄屏、按键与 IMU 唤醒）/ IMU 运动与姿态（含原始数据读取）/ 看门狗（喂狗超时自动重启）/ 通知（持久化 + 提示音）/ OTA(HTTPS) / MQTT / WebSocket / 系统信息（崩溃记录、任务 CPU、最近日志环、硬件与固件信息）/ 串口命令行 / 摄像头（按需开关，把 esp32-camera 类型挡在服务层内） |
| `framework/` | `fw_app_mgr`、`fw_window`、`fw_input`、`fw_theme`、`fw_asset`、`fw_ui`、`fw_statusbar`、`fw_notification`、`fw_control_center`、`fw_boot_animation` | 已实现（状态栏集成返回 / 主页 / 通知 / 控制中心按钮与 Wi-Fi / 蓝牙 / 亮度图标；控制中心含 Wi-Fi、蓝牙、手电筒、锁定磁贴与亮度 / 音量滑块、试听，手电筒用屏幕背光实现（板载无可控 LED）；通知中心支持点击打开与长按单条撤销；锁屏浮层（上滑或长按解锁）；App 支持启动参数与 URI 启动；中文由 assets 的 Noto 子集 14/16 px 渲染；开机画面用官方 Logo + 提示音） |
| `apps/` | `app_home` + PRD 3.11 的 17 个内置 App：`app_clock`、`app_settings`、`app_music`、`app_recorder`、`app_image`、`app_video`、`app_camera`、`app_file`、`app_editor`、`app_calc`、`app_imu`、`app_ble`、`app_browser`、`app_ota`、`app_debug`、`app_about`、`app_factory` | **17 个全部实现**（详见各 App 文件头）。已知差距：Video 只做帧序列播放（真 MJPEG 等 MM-009 的 JPEG 解码器）；Browser 只支持 http://（`svc_http_get` 没挂证书）；OTA 只支持 HTTPS URL（本地 .bin 需要 Services 再开 API）；Editor 软键盘只有拉丁字符；Factory 的"BOOT 长按启动"属于启动路径，还没接 |

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

### 4.2 LVGL framebuffer 放内部 DMA 内存，不放 PSRAM

`lvgl_port_display_cfg_t.flags.buff_dma = true` + `buff_spiram = false`（10 行 = 320*10*2 = 6.4 KB，单缓冲）。

原因：ESP32-S3 的 `esp_ptr_dma_capable()` 只覆盖内部 DRAM（`SOC_DMA_LOW~SOC_DMA_HIGH` = 0x3FC88000~0x3FD00000），**PSRAM 不在其中**，所以帧缓冲放 PSRAM 时 SPI 驱动会为**每一笔 flush** 临时 `heap_caps_aligned_alloc(..., MALLOC_CAP_DMA)` 一块同样大小的内部回弹缓冲（本板 `max_transfer_sz` 是整屏，一次 flush 就是 10 KB 级）。Wi-Fi / BLE 起来后这笔分配会失败 → `panel_io_spi_tx_color: spi transmit (queue) color failed` → 刷屏失败；而 `esp_lvgl_port` 自带的 flush 回调忽略返回值、也不调 `lv_disp_flush_ready()`，单缓冲下 LVGL 会在 `lv_refr.c` 里 `while(draw_buf->flushing)` 死等这块缓冲 —— 表现是 LVGL 任务占满 CPU、界面永久卡住、事件总线任务被饿死触发看门狗。`periph_lcd_flush_cb()` 覆盖了它的 flush 回调，失败时补一次 `lv_disp_flush_ready()`（最多丢一帧，绝不死锁）并打印错误码。

`docs/01-requirements/01-hardware-spec.md` 里写的是"帧缓冲放 PSRAM"，那是冻结的意图文档；实测结论以本节为准（冲突处理见第九节）。

`CONFIG_LV_MEM_CUSTOM=y`、`CONFIG_LV_USE_PNG=y`、`CONFIG_LV_USE_GIF=y`（目标配置见 `docs/01-requirements/01-hardware-spec.md`）：PNG/GIF 解码器会明显增大固件体积，调整前先评估 flash 预算。

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

**只用 BLE 4.2**（`CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y` 且 `# CONFIG_BT_BLE_50_FEATURES_SUPPORTED is not set`，两行必须一起写）。IDF 默认把 5.0 特性打开，而扩展广播 / 周期广播会明显增加 BLE 控制器内存：本板实测会在 `svc_bt_init()` 里打印 `BLE_INIT: Malloc failed`、随后控制器断言并重启循环。4.2 下 `esp_ble_gap_start_advertising()` 这类 4.2 接口可用、扩展广播不可用；这些 API 本身就在 `esp_gap_ble_api.h` 的 `#if (BLE_42_FEATURE_SUPPORT == TRUE)` 里。

BLE 还有**内存余量**要求：控制器初始化要一块 30 KB 连续内存（不足时断言在 `emi.c`），主机（Bluedroid）的 `btu_workqueue`、任务栈、队列又**只能用内部 RAM**（FreeRTOS 的对象不能放 PSRAM）。实测把 BLE 放在首屏之后（Wi-Fi / LVGL / 音频已占过内部 RAM）时，同一份固件不同次启动会随机在控制器侧（`BLE_INIT: Malloc failed`）或主机侧（`Bluedroid Initialize Fail` / `Unable to allocate resources for bt_workqueue`）失败。

**结论：`svc_bt_init()` 必须放在 `services_init()` 的早期（Storage 之后、Time / Wi-Fi 之前）**，趁内部内存最干净时初始化；代价是首屏时间增加约 0.4 s。配套的 `CONFIG_BT_ALLOCATION_FROM_SPIRAM_FIRST=y`（把主机侧动态数据放到 PSRAM；控制器的分配固定用 `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`，不受它影响）+ `CONFIG_BT_ACL_CONNECTIONS=2` 继续保留。

**`CONFIG_BT_CTRL_BLE_MAX_ACT` 保持 IDF 默认 6，不要下调**：这个值（`ble_max_act`）同时决定控制器"环境内存池"的大小。实测改成 2 之后，`esp_ble_gap_start_advertising()` 被控制器以 **HCI `0x200a` status `0x07` Memory Full** 拒收（`btm_ble_write_adv_enable_complete failed`，LL 里 `ble_ll_adv_sm_get()` 取不到 adv 环境就返回 `BLE_ERR_MEM_CAPACITY`），表现为 BLE App 显示"广播中"但手机搜不到设备。当初下调是配合"BLE 晚初始化"的省内存措施；现在 BLE 已经提前到 Wi-Fi 之前，不缺这块内存。

这个失败还带**时序性**：同一份固件不同次启动，有时控制器断言（`emi.c`）、有时主机返回失败（`Bluedroid Initialize Fail`），甚至"手动点一下界面"改变了 LVGL / 音频的时序就不复现 —— 本质是与 Wi-Fi / SNTP / 音频**同时分配**内部内存的竞态，所以不能靠"晚点再重试"绕过。`svc_bt_init()` 现在会先打印 `init: internal heap free=... largest=...`，其中 **largest（最大连续块）** 才是控制器 30 KB 请求能否满足的关键。

`svc_bt_init()` 在控制器初始化前调用 `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)` 把经典蓝牙的内存还给堆；`esp_bluedroid_init/enable` 声明在 `esp_bt_main.h`（不是 `esp_bluedroid_api.h`）。

BLE HID 设备（NET-006）在 `svc_bt_hid`：协议本体移植自官方例程 `ble_hid_device_demo`，但已按本项目约定改装 —— 文件为 `svc_bt_hid_dev.c/h`（GATT 服务本体与属性表）、`svc_bt_hid_send.c/h`（报告发送）、`svc_bt_hid_report.c/h`（报告构造与用法值），符号统一 `svc_bt_hid_*` / `SVC_BT_HID_*`，TAG 统一 `svc.bt.hid`，文件头改中文并保留上游 SPDX 与原许可。只有 HID 规范里的名字（`HID_KEY_*`、`HID_CONSUMER_*`、`HID_RPT_ID_*`、`HID_REPORT_TYPE_*`、`HID_PROTOCOL_MODE_*` 等用法值与报告类型）和 `ATT_SVC_HID` 保持原名，行内注释保留英文原文以便与上游比对。功能上只有 4 处改动：分发函数改名并导出为 `svc_bt_hid_dev_gatts_event()`、`svc_bt_hid_dev_register_cb()` 不再自行注册 GATTS 回调、事件分发只认 `SVC_BT_HID_APP_ID`、删掉上游从未被调用的 `svc_bt_hid_dev_init()`（只清一次环境变量，全仓无人引用）。**Bluedroid 只允许一个 GATTS 回调**（GAP 同理），所以回调统一由 `svc_bt` 注册，事件再从 `svc_bt.c` 转发给 HID；将来新增 GATT 服务必须走这条"一个回调 + 按 app_id / gatts_if 分发"的路，否则后注册的会覆盖先注册的。HID 报告要等配对（加密）完成才能发，`svc_bt_hid_is_ready()` 反映这个状态。

两个**初始化时序 / 参数**坑（都曾表现为"状态停在 `state -> 1`、手机搜不到设备"）：

- `svc_bt_hid_gatts_event()` / `svc_bt_hid_gap_event()` 的转发开关必须**在注册 app 之前**置位。`esp_ble_gatts_app_register()` 是异步的，BTC 任务优先级又高于调用者，REG 事件常常在函数返回前就回调进来；若此时开关还没打开，事件被丢弃，`svc_bt_hid_dev_cb_handler()` 拿不到 `ESP_GATTS_REG_EVT` 就不会创建服务（现场只有 `initializing`，没有 `hid svc handle` / `hid device registered`）。
- 广播数据里的 **`service_uuid_len` 必须是 16 的整数倍**：`esp_ble_gap_config_adv_data()` 里有 `if (service_uuid_len & 0xf) return ESP_ERR_INVALID_ARG;`，直接传 2 字节的 16 位 UUID 会被拒收（现场 `config adv data failed: ESP_ERR_INVALID_ARG`，于是 `ADV_DATA_SET_COMPLETE` 永远不来、状态停在 READY）。要广播 16 位服务 UUID（如 HID `0x1812`）必须用 16 字节的 128 位形式（Bluetooth base UUID，`[12]/[13]` 就是那两个字节），官方 HID 例程也是这么写的。
- 广播 / 扫描**不依赖 GAP 完成事件**：把 `esp_ble_gap_config_adv_data()` / `esp_ble_gap_set_scan_params()` 返回 `ESP_OK` 当作就绪（`s_adv_ready` / `s_scan_ready`），顺序由 BTA / BTU 单队列保证（数据写入先于开启命令）。`ADV_START_COMPLETE` 到达时仍用于确认，失败则回退到 READY 并按 3 s / 最多 5 次重试（控制器偶发给不出环境时能自愈）。
- **连接 / 断开事件会按"每个注册过的 GATT app"各回调一次**（`svc_bt` 的自定义服务 + HID `0x1812` + 电量 `0x180f`，一次连接就会有 3 个 `ESP_GATTS_CONNECT_EVT`，`gatts_if` 各不相同）。所以：`gatts_cb` 里的连接 / 断开处理也必须按 `gatts_if != s_gatts_if` 过滤，`svc_bt_note_conn()` 也要幂等（否则会重复打日志、重复发好几次开广播请求）。

排查入口（都在启动日志 / 串口命令里）：

- `callbacks: gatts=... gap=... match=x/y` —— 用 `esp_ble_gatts_get_callback()` / `esp_ble_gap_get_callback()` 读出协议栈里**真正生效**的回调指针，与本文件的 `gatts_cb` / `gap_cb` 比较；`match` 不是 1/1 说明注册没成功。
- `gap event N` —— 每个非扫描结果的 GAP 事件（`ESP_LOGD`，默认的 `CONFIG_LOG_MAXIMUM_LEVEL_INFO` 下编译期就被裁掉；要用得把日志级别调到 Debug）。
- `selftest: state=... adv(ready/active) scan_ready=... gap_events=N last=E` —— `svc_bt_init()` 后 2 s 自动打印一次，只看串口日志也能判断蓝牙到底在不在广播。
- 串口命令 `bt` —— 打印上面同一组数据（`svc_bt_get_diag()`）。

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

### 4.17 看门狗：启动完成后才打开自动重启

Task WDT 由 IDF 启动时初始化（`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`，默认**只告警不重启**）。`svc_watchdog_init()` 在 Services 初始化时把它配成"只告警"，`main.c` 在**内置 Flash 挂载（首次 SPIFFS 格式化）之后**才调 `svc_watchdog_arm()` 打开"喂狗超时自动重启"（PRD SYS-004）。不要把这个调用提前：格式化 3 MB 存储分区期间任务长时间不喂狗，提前打开会重启 → 格式化永远做不完 → 启动循环。

纳入监控的任务必须在自己的循环里 `svc_watchdog_subscribe()`（幂等）+ `svc_watchdog_feed()`，且循环周期远小于超时。当前纳入：事件总线派发任务、`svc_power`、`svc_imu`。**长时间阻塞在队列上的任务不要直接纳入**：`svc_audio`、`ntp_sync_task` 尚未纳入，要先改成"有限等待 + 喂狗"才安全。超时可用 `svc_watchdog_set_timeout()` 或 `CONFIG_ESP_TASK_WDT_TIMEOUT_S` 调整。

### 4.18 崩溃记录：RTC 暂存 + wrap panic handler

`svc_sysinfo` 负责 SYS-005：崩溃时把现场（任务名 / 调用栈 PC / SP / 内存 / 运行时长）写进 **RTC 不初始化内存**（`RTC_NOINIT_ATTR`），下次启动读出来写 NVS（`sys/crash_log`，只保留最近 3 条）并用 `ESP_LOGE` 打到串口。不用 `espcoredump`：它需要额外的 coredump 分区，而分区表是冻结的。

抓现场靠链接期 wrap IDF 的 panic handler：`services/CMakeLists.txt` 的 `-Wl,--wrap=esp_panic_handler` + `svc_sysinfo.c` 的 `__wrap_esp_panic_handler()`。注意三点：只能拦到走 panic handler 的崩溃（`panic_abort`、断言等），**cache 关闭时崩溃**（如 flash 操作）wrap 函数在 flash 里可能来不及写，这种情况启动时只报 `reset=panic (no detail)`（复位原因兜底）；包装函数必须 `IRAM_ATTR` 且不能调 libc（字符串自己搬）；启动时若 `esp_reset_reason()` 是 panic / 看门狗 / brownout，即使没有详细现场也会记一条。

panic 上下文里**不要碰堆**：`heap_caps_get_free_size()` 可能正持着堆锁，会造成二次崩溃（串口表现为 `Panic handler entered multiple times`、且没有 Guru Meditation 输出），所以现场里不记录崩溃瞬间的堆占用；包装函数只做无锁操作 —— RTC 写入、`esp_timer_get_time()`、`pcTaskGetName()`、回溯帧遍历。

DBG-003 的任务 CPU 占用依赖 FreeRTOS 运行时统计，`sdkconfig.defaults` 已打开 `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`（同时打开 `CONFIG_FREERTOS_USE_TRACE_FACILITY`，否则 `uxTaskGetSystemState()` 不可用）。`svc_sysinfo_get_tasks()` 第一次调用只建基准，所有任务的 CPU 百分比都是 0。

验收这条路可以直接用串口命令 `crash`（`svc_shell`，会 `abort()` 一次）：重启后串口应出现 `svc.sysinfo: last crash: reset=panic ... task=... pc=...`，再用 `svc_sysinfo_get_crash_log()` 读 NVS 里的记录。

### 4.19 锁屏：全屏浮层 + 手势路由只在 fw_input

`fw_lockscreen`（Framework）是一个盖住整屏（含状态栏）的浮层，显示时钟与"已锁定 上滑解锁"。解锁两条路：上滑（`periph_touch` 识别 → `svc_power` 发布 `SVC_EVENT_GESTURE_SWIPE_UP` → `fw_input` 路由）或长按浮层本身（便于排查手势）。触发入口是控制中心的"锁定"磁贴；`fw_init` 在控制中心 / 通知中心之后初始化它，保证它是 `lv_layer_top()` 上的最上层浮层。

滑动手势的消费全部集中在 `fw_input`：下拉 = 通知中心、上滑 = 控制中心（锁屏时改为解锁）、左右 = 返回（先收浮层再返回）。锁屏时 BOOT 键只保留长按（电源菜单），其它按键与手势都被忽略，避免绕过锁屏。PRD / UI 规范里没有锁屏条目，行为按上面这套定义。

### 4.20 GCC 14 与第三方旧 C 代码

工具链是 GCC 14（xtensa-esp-elf 14.2.0），它把 `-Wincompatible-pointer-types`、`-Wint-conversion`、`-Wimplicit-function-declaration`、`-Wimplicit-int`、`-Wreturn-mismatch` 这一批从警告默认升级成**错误**。我们依赖的托管组件里就有旧 C 代码会因此挂掉：LVGL 8.3.0 的 `src/extra/libs/png/lv_png.c` 把 `uint32_t *` 传给 lodepng 的 `unsigned *` 形参。

`CONFIG_COMPILER_DISABLE_GCC14_WARNINGS`（已在 `sdkconfig.defaults` 打开）在 IDF 5.4 里只加 `-Wno-calloc-transposed-args`，覆盖不到上面这几项，所以真正的处理在顶层 `CMakeLists.txt`：遍历构建组件，**只对托管组件**（名字形如 `namespace__name`）加 `-Wno-error=...`；自己的 `drivers/peripherals/services/framework/apps` 不加，保持严格编译。不要去改 `managed_components/` 里的文件 —— 那是构建产物，重新 sync 依赖会被覆盖。

### 4.21 四个自检脚本（改完代码 / 文案跑一遍）

都不依赖硬件，也不需要构建（GCC 14 把很多问题从警告升级成错误，静态自检能提前拦一部分）：

- `python tools/check_cn_text.py` —— 界面文案用到的汉字是否都在字表里（缺字就加进 `tools/cn_chars.py`，再 `python tools/gen_cn_font.py --sizes 14,16` 重新生成字体）
- `python tools/check_ui_colors.py` —— 创建了标签却没显式设色的地方（浅色主题下会白字白底看不见）
- `python tools/check_api_includes.py` —— 调用了别的层的函数，但声明它的头文件不可见（GCC 14 下是错误；踩过 `periph_camera.c` 缺 `drv_common.h`）
- `python tools/check_decl_order.py` —— 文件内 static 定义晚于使用（踩过 `svc_bt_hid.c` 把 static 块挪到 `svc_bt_hid_init()` 之后）
- `python tools/check_lvgl_api.py` —— App 层用到的 LVGL 标识符在 `managed_components/lvgl__lvgl` 里是否存在（踩过 8.3 没有 `LV_LABEL_LONG_SCROLL_RIGHT`），顺便列出带 `%s` 的 `snprintf` 供人工确认截断风险（GCC 14 的 `-Werror=format-truncation` 会把 256 字节的 `svc_storage_entry_t.name` 塞进小缓冲直接判错）

### 4.22 内部 RAM 很紧，动配置前先算账

内部 SRAM 只有 190 KB 可用堆，而下面这些**只能用内部 RAM**的消费者加起来已经把大部分吃掉了：Wi-Fi（静态收发缓冲 + WPA 派生）、BLE（控制器环境池 + 主机任务/队列）、音频 I2S DMA、LVGL 帧缓冲（6.4 KB，见 4.2）、各服务任务与队列、`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 预留的一块。

实测踩过的坑（`del sdkconfig` 后按新配置重建的那一次）：

- 表现是**在 Wi-Fi 关联 + SPIFFS 挂载那个时间窗**里连小对象都分配不出来：
  `E SPIFFS: mutex lock could not be created` 后 `assert vQueueDelete`（`periph_storage_mount`），
  或者 `abort() at lock_init_generic`（`wpa_set_passphrase` 里的 newlib 锁）。两者都是"内部堆真的空了"，不是配置错。
- 触发它的三件事叠在一起：`CONFIG_BT_CTRL_BLE_MAX_ACT` 从 2 恢复到 6（控制器环境池变大，见 4.6）、
  广播真的开始工作、帧缓冲从 PSRAM 搬到内部 DMA（4.2）。
- 对策（都在 `sdkconfig.defaults`）：`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=16384`（默认 32768，
  大块 DMA 缓冲都在开机早期分配，运行期只有小请求）、`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8` /
  `STATIC_TX_BUFFER_NUM=8`（默认 16/16，每块约 1.6 KB 且只能用内部 RAM）、LVGL 帧缓冲降到 10 行。
- **17 个 App 落地后又撞了一次同类 OOM**（Wi-Fi 算 WPA PSK 时 `lock_init_generic` → `abort`）：启动可见堆从
  190 KB 掉到 156 KB —— App 的静态数组 + 桌面上 18 个图标的 LVGL 对象（LVGL 走普通 malloc，小于阈值就落
  内部 RAM）一起吃掉约 34 KB。两条对策：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`（通用 malloc 一律优先
  PSRAM；FreeRTOS 对象 / 任务栈 / DMA 缓冲走 `MALLOC_CAP_INTERNAL`，不受影响，BLE 的队列与栈仍在内部），
  以及把 App 里的大静态数组清掉（Browser 的 6 KB 响应缓冲改成运行期 malloc、Editor 的路径表 3 KB → 2 KB、
  Video 的目录表 1.5 KB → 1.2 KB、Music / Recorder 列表长度减半）。**新增 App 时 static 数组别超过几百字节。**
- 改完之后的实测数字（可用作基准）：BLE 初始化前 `dma-internal free=156763 largest=110592`；
  **BLE 控制器 + 广播环境要吃掉约 52 KB**（`init done` 时 free=104219）；最紧的时刻是
  "Wi-Fi 刚连上 + 首屏 + SPIFFS 挂载"这一点，`free≈18 KB`，之后就稳定了；PSRAM 始终有 8.2 MB 富余。
  也就是说内部 RAM 的余量只有十几 KB，往内部塞任何新的大块缓冲前必须重新称重。

**诊断入口**（一律用 `MALLOC_CAP_DMA` 统计：它正好是"内部 DMA 可用区"，也是 BLE / 音频 / 帧缓冲 /
FreeRTOS 对象真正会耗的那块；`MALLOC_CAP_INTERNAL` 会把 IRAM 算进来，`largest` 会得出比 `free` 还大的怪值）：

- `main.c` 挂载内置 SPIFFS 前：`heap before internal mount: dma-internal free=... largest=...`
- `svc_bt_init()` 开始 / 结束：`init: dma-internal heap free=...` / `init done: dma-internal heap free=...`
- BLE 自检（+2 s）：`selftest: dma-internal heap free=...`
- 串口 `sysinfo`：当前堆余量

**已知待办**：`periph.audio` 的麦克风自检一直读到满量程（`peak 32767/32767`），说明 I2S RX 没拿到真实
数据（ES7210 的 I2C 配置是通的，怀疑串口格式 / 增益 / 通道映射）。录音 App 已能录出带 WAV 头的文件，
但音质要等这个修好才算通过验收（"录音后文件可在 PC 播放"）。

### 4.23 新增 App 用到的公共设施（不要各写一套）

写 App 前先看这一节，能省掉大量重复代码，也避免各 App 视觉不一致：

| 用途 | 用谁 |
|------|------|
| 页面根屏 + 内容容器（从状态栏下方开始、内边距 12、行距 8） | `fw_ui_page(&content)` |
| 整行入口（高 50、卡片底 + 描边，右侧可显示数值） | `fw_ui_row_btn()` + `fw_ui_row_btn_value()` |
| "标签 + 滑块"一行 | `fw_ui_slider_row()` |
| 列表 / 网格 / Toast / 对话框 / 进度条 | `fw_ui_list()`、`fw_ui_list_add()`、`fw_ui_grid()`、`fw_ui_toast()`、`fw_ui_dialog()`、`fw_ui_progress_bar()` |
| 主题色 | 只用 `fw_theme_color_*()`（见 4.14） |
| 字体 / 图标 | `fw_asset_font_cn()/cn_large()/14()/20()/24()`、`fw_asset_symbol_for(app_name)` |
| LVGL 显示图片 / GIF（按文件路径） | `fw_asset_fs_path()` 转成 `"A:/sdcard/..."` 再给 `lv_img_set_src()`；FS 驱动在 `fw_asset_init()` 里注册（POSIX 读 VFS，只读） |
| 目录遍历 | `svc_storage_iter_start/next/end`（`iter_next` 返回的是内部缓冲，用完必须马上拷走） |
| 整块读写小文件（编辑器的文本、相机的 JPEG） | `svc_storage_read/write/remove/exists`（读上限 1 MB） |
| 摄像头 | `svc_camera_*`（不要把 `esp_camera.h` 引进 App） |
| 系统信息 / 最近日志 / 任务 CPU | `svc_sysinfo_get()`、`svc_sysinfo_get_recent_logs()`、`svc_sysinfo_get_tasks()` |

两条硬约束：**App 不直接调 IDF / Peripherals / Drivers**（缺接口就往 Services 加薄封装）；**不要在 App 里加大块 static 缓冲**（内部 RAM 只有十几 KB 余量，见 4.22）。

每写完一批 App：`python tools/check_cn_text.py`（新文案的字加进 `tools/cn_chars.py` 后 `python tools/gen_cn_font.py --sizes 14,16`）、`check_ui_colors.py`、`check_api_includes.py`、`check_decl_order.py` 四个脚本跑一遍。

### 4.24 往日志里挂钩子必须做重入保护

`svc_sysinfo` 为了给 Debug App 提供"最近日志"，用 `esp_log_set_vprintf()` 装了钩子：把每行抄进无锁环，再调用原来的输出函数。这个钩子必须自己防重入，因为**日志输出路径内部会再打日志**：`uart_write_bytes()` 开头的 `ESP_RETURN_ON_FALSE` 一旦条件不满足就 `ESP_LOGE`，这条日志又走输出 → 回到钩子 → 再写 UART → 再失败……**无限递归**，实测表现是满屏重复回溯 + `assert xQueueSemaphoreTake (pxQueue->uxItemSize == 0)`（栈被压爆后写坏了 FreeRTOS 对象），而且因为 panic 里又打日志，重启循环。

做法（见 `svc_sysinfo.c`）：用"当前任务是否已在本函数里"判断重入（`xTaskGetCurrentTaskHandle()` 与保存的 owner 比对，重入直接 `return 0`），行缓冲放**静态区**而不是调用者栈上，并且**剩余栈不足时不抄录**（`uxTaskGetStackHighWaterMark(NULL) < 512` 就跳过）—— `vsnprintf` 的栈开销算在调用者头上，BTC_TASK（3072 B，被 Bluedroid 日志吃到临界）实测会被压爆栈；跳过抄录后原有输出路径的栈开销不变。实在还溢出再抬 `CONFIG_BT_BTC_TASK_STACK_SIZE`。

界面侧配套的坑：**不要在定时刷新的页面上放超大自动换行标签**。Debug App 最初每 2 s 刷新一个 1.2 KB 的 `LV_LABEL_LONG_WRAP` 标签并 `lv_obj_scroll_to_view()`，LVGL 任务会长时间卡在 `draw_scrollbar → lv_obj_get_self_height → lv_label 布局 → 字体 glyph 查询`，把同核的 `svc_imu` 饿死 → 看门狗 abort。现在只显示最近 512 B、刷新间隔 3 s、不再自动滚动。

---

## 五、存储与分区预算（不要超）

| 资源 | 容量 | 用途 |
|------|------|------|
| Flash | 16 MB | 全部固件 + 资源 |
| PSRAM | 8 MB Octal @ 80 MHz | 图片 / 视频 / 音频大 buffer、LVGL 对象内存 |

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
4. services_init()            → Watchdog, EventBus, Settings, Storage, BT, Time, Audio, Net, Power, IMU, Noti, SysInfo, Shell
5. fw_init()                  → Theme, Asset, Window, AppMgr, UI, StatusBar, NotiCenter, CtrlCenter, Input
6. app_register_all()         → 注册所有内置 App
7. fw_boot_animation()        → 全屏官方 Logo 静态展示 + 单声开机提示音（约 1.7 s）
8. fw_app_mgr_launch("Home")  → 显示桌面
9. periph_storage_mount(内置) → 首屏之后挂载内置 SPIFFS（首次自动格式化）
10. svc_watchdog_arm()        → 启动完成，打开看门狗超时自动重启

（未实现：P2 项（i18n / 屏幕旋转 / 按键映射 / 通知优先级与免打扰 / NVS 加密 / 视频播放 / IMU 自动旋转与计步 / 应用权限与沙箱强制）按需求优先级留到后续；Apps 层还有 13 个规划中的 App）
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

### IMU 运动 / 姿态
periph_imu（svc_imu 任务，50 ms 轮询；QMI8658 中断引脚未引出）→ 判定运动 / 朝向 → svc_event_bus 发布 `SVC_EVENT_IMU_MOTION` / `SVC_EVENT_IMU_ORIENTATION` → svc_power 熄屏时唤醒

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