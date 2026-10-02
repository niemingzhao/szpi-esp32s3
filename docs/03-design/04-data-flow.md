# 数据流与场景分析

本文档通过典型场景，展示 SZPI-OS 各层之间如何协作完成端到端功能。

## 1. 开机启动

```
[插入电源 / 按下复位键]
        │
        ▼
   [ESP32 引导]
        │
        ▼
   [app_main()]
        │
        ├─→ bsp_init()              // I2C, LEDC, PCA9557, LCD, Touch, Key, IMU
        ├─→ peripherals_init_all()  // IO, Audio, LCD(+LVGL), Touch, IMU, Storage, Button, Ext
        │     │
        │     └─→ periph_lcd_init() 内部初始化 LVGL 并注册显示
        │           periph_touch_init() 注册 LVGL input device
        │
        ├─→ services_init()         // Watchdog, EventBus, Settings, Storage, BT, Time,
        │                           //   Audio, Net, Power, IMU, IO, Camera, SysInfo
        │
        ├─→ fw_init()               // Theme, Asset, Window, AppMgr, Script, UI, StatusBar, Input
        │
        ├─→ app_register_all()      // 注册所有内置 App
        │
        ├─→ fw_boot_animation()     // Logo + 提示音（约 2.05 s）
        │
        ├─→ fw_app_mgr_launch("Home")
        │     │
        │     └─→ home_on_create() → 显示桌面
        │
        ├─→ periph_storage_mount(内置 Flash)   // 首屏之后挂载（首次格式化）
        │
        └─→ svc_watchdog_arm()      // 打开超时自动重启
```

### 1.1 时间预算

| 阶段 | 目标 | 实测 |
|------|------|------|
| ROM + bootloader + IDF 启动（含 PSRAM 自检约 300 ms） | 约 1.2 s | 1.20 s |
| `bsp_init` | < 250 ms | 190 ms |
| `peripherals_init_all`（含 TF 卡挂载） | < 400 ms | 280 ms |
| `services_init`（含蓝牙约 150 ms、Wi-Fi 约 100 ms） | < 450 ms | 320 ms |
| `fw_init` | < 150 ms | 100 ms |
| `app_register_all`（23 个 App） | < 150 ms | 90 ms |
| `fw_boot_animation`（Logo 静态展示 + 提示音） | 约 2.05 s | 2.14 s |
| 创建并启动桌面 | < 150 ms | 90 ms |
| **总计到桌面（从供电起算）** | **< 5 s** | **约 4.4 s** |

内置 SPIFFS 在桌面显示之后才挂载（不做 OTA 的 7 MB 分区整块挂载实测约 0.8 s），不计入上表。

## 2. 触摸点击进入 App

```
[FT6336 轮询检测到触摸]
        │
        ▼
[esp_lcd_touch 读取坐标]
        │
        ▼
[periph_touch 扫描任务：读取 + 缓存 + 点击 / 长按判定]
        │
        ├─→ periph_touch 回调（PRESS / RELEASE / TAP / LONG_PRESS）
        │
        ▼
[LVGL indev read_cb 读取缓存坐标]
        │
        ▼
[LVGL input device 派发到当前屏]
        │
        ▼
[桌面处理事件 → 点中 "音乐" 图标]
        │
        ▼
[fw_app_mgr_launch("Music")]
        │
        ├─→ Music 是否已创建？
        │     ├─ 是 → music_on_resume()
        │     └─ 否 → music_on_create() → music_on_start()
        │
        └─→ fw_window_switch_to(music_root, FADE_IN)
              │
              └─→ 250 ms 淡入，Music 完全显示
```

## 3. 运行脚本

```
[用户进入脚本管理，选中一个脚本]
        │
        ▼
[app_scripts 调用 fw_script_run(path)]
        │
        ▼
[fw_script（script_task）加载 Lua 脚本]
        │
        ├─→ 脚本调用绑定：
        │     ├─ 界面 / 输入 / 通知 → fw_ui / fw_input
        │     ├─ 音频 / 摄像头 / 文件 / 网络 / BLE / 系统 → 各 Services
        │     └─ GPIO / PWM / I2C / UART / ADC → svc_io
        │
        ├─→ 脚本运行中：定时器 / 事件回调在 script_task 上下文执行
        │
        └─→ 用户停止 或 脚本异常退出
              │
              ├─→ 释放脚本创建的界面对象、定时器、订阅
              ├─→ 发布 SVC_EVENT_SCRIPT_STOPPED
              └─→ 回到脚本管理
```

## 4. 音乐播放

```
[用户点击 "播放" 按钮]
        │
        ▼
[music App 中 btn_play_cb]
        │
        ▼
[svc_audio_play(&src)]
        │
        ├─→ 请求入队
        │
        ▼
[play_task 取出请求]
        │
        ├─→ 打开文件（/sdcard/music/xxx.mp3）
        ├─→ helix MP3 解码器初始化
        ├─→ periph_audio_set_format(PLAY, ...)
        │
        ▼
[play_task 循环]
        │
        ├─→ 读文件块 → 解码 → PCM 写入 periph_audio_write()
        │                                    │
        │                                    ▼
        │                              I2S0 TX → ES8311 → NS4150B → 喇叭
        │
        └─→ 播放完成 → 回调 → SVC_EVENT_AUDIO_PLAYBACK_FINISHED
              │
              └─→ 状态栏监听 → 更新播放图标
```

## 5. Wi-Fi 连接

```
[用户进入 Wi-Fi App]
        │
        ├─→ svc_net_wifi_scan(...) → 显示 AP 列表
        │
        ▼
[用户点击 "HomeWi-Fi" 并输入密码]
        │
        ▼
[svc_net_wifi_connect(&creds)]
        │
        ├─→ ESP-IDF Wi-Fi 事件回调
        │
        ├─→ 失败 → SVC_EVENT_WIFI_CONNECT_FAILED → 界面提示
        │
        └─→ 成功 → SVC_EVENT_WIFI_CONNECTED
              │
              ├─→ svc_net_get_status() 获取 IP
              ├─→ svc_time_sync_ntp() 同步时间
              ├─→ 状态栏更新 Wi-Fi 图标
              └─→ fw_ui_toast("Wi-Fi 已连接")
```

## 6. 摄像头拍照

```
[用户进入相机 App，点拍照]
        │
        ▼
[cam App 调用 svc_camera_open() / svc_camera_capture(...)]
        │
        ├─→ drv_camera（esp32-camera）取一帧 RGB565（PSRAM）
        │
        ▼
[cam App 把帧编码为 BMP]
        │
        ▼
[svc_storage_write("/sdcard/DCIM/IMG_xxx.bmp", data, len)]
        │
        ▼
[svc_camera_release(buf)]
        │
        ▼
[fw_ui_toast("已保存到 DCIM")]
```

拍照期间预览暂停；GC0308 / GC2145 由 esp32-camera 按 PID 自动识别。

## 7. 屏幕超时熄屏 → 唤醒

```
[power_task 每秒检查]
        │
        ├─→ 距上次触摸已过 N 秒？
        │     │
        │     └─→ 是 → svc_power_sleep()
        │              │
        │              └─→ periph_lcd_set_backlight(false)（只关背光，LCD 保持）
        │
        ▼
[触摸 / 按键 / 抬手发生]
        │
        ├─→ 触摸 → SVC_EVENT_TOUCH
        │     按键 → fw_input 回调
        │     抬手 → SVC_EVENT_IMU_PICKUP
        │           │
        │           └─→ svc_power 监听 → svc_power_wake()
        │                 │
        │                 └─→ periph_lcd_set_backlight(true)
        │
        └─→ svc_power 重置超时计数
```

摇晃 / 运动（`SVC_EVENT_IMU_SHAKE` / `SVC_EVENT_IMU_MOTION`）**不唤醒**：太容易被碰到
就亮屏。只保留抬手：设备平放且静止（|z 占比| ≥ 0.92）≥ 1 s 后，屏幕立起来（占比 ≤ 0.87，
即倾斜约 30° 以上）并保持 200 ms，才发 `SVC_EVENT_IMU_PICKUP`。

## 8. 电源菜单（长按 BOOT）

```
[BOOT 按键按住达 1.5 s]
        │
        ▼
[drv_key 轮询任务在按住期间直接上报 LONG_PRESS（不用松手）]
        │
        ▼
[fw_input 全局回调 → 弹出电源菜单浮层]
        │
        ├─→ [重启] → svc_power_request_reboot() → esp_restart()
        ├─→ [关机] → svc_power_request_shutdown() → 熄屏待机
        └─→ [取消] → 关闭浮层
```

## 9. 性能预算汇总

| 场景 | 目标 |
|------|------|
| 开机到桌面 | < 5 s |
| App 切换 | < 500 ms |
| 脚本启动 | < 500 ms |
| 触摸响应 | < 100 ms |
| Wi-Fi 连接（已知） | < 5 s |
| 拍照保存 | < 1 s |
| MP3 启动 | < 300 ms |
| UI 帧率 | ≥ 30 FPS |

## 10. 关键路径与瓶颈

### 10.1 关键路径（必须流畅）

- **触摸 → UI**：FT6336 → periph_touch 缓存 → LVGL input device → 当前界面事件回调
- **音频播放**：play_task → 解码 → I2S0 → ES8311
- **显示刷新**：LVGL tick → render → LCD

### 10.2 潜在瓶颈

- **SRAM**：512 KB 内置，任务栈 + LVGL + 蓝牙占用紧张
- **PSRAM**：8 MB，图片 / 音频 / 摄像头缓冲占大头
- **音频 + UI 同时**：核心 1 跑音频解码，核心 0 跑 LVGL，互不干扰
- **脚本**：单个脚本的运行内存要控制，避免挤占内部 RAM

## 11. 数据流图（综合）

```
┌──────────────────────────────────────────────────────┐
│ 用户交互（触摸 / 按键 / IMU）                          │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Drivers（硬件读写）                                    │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Peripherals（业务语义封装）                            │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Services（事件总线 / 业务逻辑）                        │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Framework（LVGL 封装 / 窗口 / App 管理 / 脚本运行时）  │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Apps / 脚本（具体业务）                                │
└──────────────────────────────────────────────────────┘
```

每一层通过事件总线（`svc_event_bus`）跨层通信，避免直接跨层调用。
