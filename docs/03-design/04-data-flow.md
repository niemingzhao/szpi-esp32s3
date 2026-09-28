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
        ├─→ bsp_init()           // I2C, SPI, LEDC, PCA9557, LCD, Touch, Key, IMU
        ├─→ peripherals_init_all() // IO, Audio, LCD(+LVGL), Touch, IMU, Storage, Button
        │     │
        │     └─→ periph_lcd_init() 内部 lvgl_port_init() + lvgl_port_add_disp()
        │           periph_touch_init() 注册 LVGL input device
        │
        ├─→ services_init()       // EventBus, Storage, Time, Audio, Net, Power, Noti
        │     │
        │     └─→ 启动各服务的内部任务
        │
        ├─→ fw_init()            // Theme, Asset, Window, Input, StatusBar, CtrlCenter, NotiCenter, AppMgr
        │
        ├─→ app_register_all()   // 注册所有 App
        │
        ├─→ fw_boot_animation()  // Logo 动画 1.5 s
        │
        └─→ fw_app_mgr_launch("Home")
              │
              └─→ home_on_create() → 显示桌面
```

### 1.1 时间预算

| 阶段 | 目标时间 |
|------|----------|
| `bsp_init` | < 200 ms |
| `peripherals_init_all` | < 500 ms |
| `services_init` | < 100 ms（启动后台任务） |
| `fw_init` | < 100 ms |
| `app_register_all` | < 10 ms |
| `fw_boot_animation` | 1500 ms |
| **总计到首屏** | **< 2.5 s** |

## 2. 触摸点击进入 App

```
[FT6336 轮询检测到触摸]
        │
        ▼
[esp_lcd_touch_read_data 读取坐标]
        │
        ▼
[periph_touch 扫描任务：读取 + 缓存 + 手势识别]
        │
        ├─→ periph_touch 回调（PRESS / TAP / LONG_PRESS / SWIPE...）
        │
        ▼
[LVGL indev read_cb 读取缓存坐标]
        │
        ▼
[LVGL input device 派发到当前屏]
        │
        ▼
[桌面 lv_obj 处理事件]
        │
        ├─→ 点击了 "音乐" 图标
        │
        ▼
[music_event_handler] (App 注册的 LVGL 事件回调)
        │
        ▼
[fw_app_mgr_launch("Music")]
        │
        ├─→ 检查 Music 是否已存在
        │     ├─ 是 → 调用 music_on_resume()
        │     └─ 否 → 调用 music_on_create() → music_on_start()
        │
        └─→ fw_window_switch_to(music_root, FADE_ON)
              │
              └─→ 触发淡入动画，300 ms 后 Music 完全显示
```

## 3. 音乐播放

```
[用户点击 "播放" 按钮]
        │
        ▼
[music_app 中 btn_play_cb]
        │
        ▼
[svc_audio_play(&src)]
        │
        ├─→ svc_audio 内部 queue: 请求入队
        │
        ▼
[play_task 取出请求]
        │
        ├─→ svc_storage_open("/sdcard/music/xxx.mp3")
        ├─→ helix MP3 解码器初始化
        ├─→ periph_audio_set_format(PLAY, ...)
        ├─→ periph_audio_set_volume(current_vol)
        │
        ▼
[play_task 循环]
        │
        ├─→ 读取文件块 → 解码 → PCM 写入 periph_audio_write()
        │                                       │
        │                                       ▼
        │                                  I2S1 TX → ES8311 → NS4150B → 喇叭
        │
        └─→ 播放完成 → svc_audio_cb_t 回调 → SVC_EVENT_AUDIO_PLAYBACK_FINISHED
              │
              └─→ statusbar 监听 → 更新播放图标
```

## 4. Wi-Fi 连接

```
[用户进入 Settings → Wi-Fi 设置]
        │
        ▼
[app_settings 进入 Wi-Fi 子页]
        │
        ├─→ svc_net_wifi_scan(...)
        │     │
        │     └─→ scan_task 执行扫描 (3-5 秒)
        │
        ▼
[显示 AP 列表]
        │
        ├─→ 用户点击 "HomeWi-Fi"
        │
        ▼
[弹出密码输入框]
        │
        ├─→ 用户输入密码
        │
        ▼
[svc_net_wifi_connect(&creds)]
        │
        ├─→ svc_net 内部：连接 Wi-Fi
        │     │
        │     └─→ ESP-IDF wifi 事件回调
        │
        ├─→ 失败 → SVC_EVENT_WIFI_CONNECT_FAILED → 设置页提示
        │
        └─→ 成功 → SVC_EVENT_WIFI_CONNECTED
              │
              ├─→ svc_net_get_status() 获取 IP
              ├─→ svc_time_sync_ntp() 同步时间
              ├─→ statusbar 更新 Wi-Fi 图标
              └─→ svc_notification_post("Wi-Fi 已连接")
```

## 5. 通知

```
[任意地方调用 svc_notification_post(&noti)]
        │
        ▼
[svc_notification 内部：分配 ID，存入列表]
        │
        ├─→ 立即在屏幕顶部弹出 toast（3 秒后消失）
        │
        └─→ 发布 SVC_EVENT_NOTIFICATION_POSTED
              │
              ├─→ statusbar 更新通知图标（红点）
              │
              └─→ fw_notification 监听 → 在通知中心列表中显示
                    │
                    └─→ 用户下滑屏幕 → 显示通知中心
                          │
                          ├─→ 用户点击通知 → 调用 noti.on_click()
                          │     │
                          │     └─→ 通常是 fw_app_mgr_launch(noti.target_app)
                          │
                          └─→ 用户滑动删除 → svc_notification_dismiss(id)
                                │
                                └─→ SVC_EVENT_NOTIFICATION_DISMISSED
                                      │
                                      └─→ fw_notification 移除该条目
```

## 6. 锁屏超时 → 唤醒

```
[power_task 每秒检查]
        │
        ├─→ 上次触摸后已过 N 秒？
        │     │
        │     └─→ 是 → svc_power_sleep()
        │              │
        │              ├─→ periph_lcd_set_backlight(false)
        │              └─→ periph_imu_deinit()（省电）
        │
        ▼
[下次触摸发生]
        │
        ├─→ periph_touch 扫描任务
        │     │
        │     └─→ svc_event_bus_publish(SVC_EVENT_TOUCH, ...)
        │           │
        │           └─→ svc_power 监听 → svc_power_wake()
        │                 │
        │                 ├─→ periph_lcd_set_backlight(true)
        │                 └─→ periph_imu_init()
        │
        └─→ svc_power 重置 timeout 计数
```

## 7. 电源菜单（长按 BOOT）

```
[BOOT 按键 1.5 s+]
        │
        ▼
[periph_button 任务检测 LONG_PRESS]
        │
        ▼
[fw_input 全局回调 → fw_show_power_menu()]
        │
        ▼
[LVGL 弹出 power menu 浮层]
        │
        ├─→ [关机]
        │     └─→ svc_power_request_shutdown() → deep sleep
        ├─→ [重启]
        │     └─→ svc_power_request_reboot() → esp_restart()
        └─→ [取消]
              └─→ 关闭浮层
```

## 8. OTA 升级

```
[用户进入 Settings → 系统更新]
        │
        ▼
[app_ota 调用 svc_ota_check_and_update(url)]
        │
        ├─→ 通过 HTTPS GET 检查版本
        │     │
        │     ├─→ 当前是最新 → 显示"已是最新"
        │     └─→ 有新版本 → 显示对话框 [下载更新]
        │
        ├─→ 用户确认 → 下载固件到 OTA 分区
        │     │
        │     └─→ 下载进度 → svc_notification_post("更新中...")
        │
        ├─→ 下载完成 → esp_restart() 进入新固件
        │     │
        │     └─→ 新固件启动 → 检查上次 OTA 状态
        │           │
        │           ├─→ 成功 → 删除旧固件，发送 notification
        │           └─→ 失败 → 回滚到旧固件，发送 notification
        │
        └─→ 升级失败 → 回滚 + notification 提示
```

## 9. 摄像头拍照

```
[用户进入 Camera App，按下拍照]
        │
        ▼
[cam_app 调用 periph_camera_capture(&frame)]
        │
        ├─→ drv_gc0308_grab_frame()
        │     │
        │     └─→ esp_camera_fb_get()  (DVP → PSRAM)
        │
        ▼
[cam_app 转换/编码 JPEG]
        │
        ├─→ frame2jpg() → JPEG 数据
        │
        ▼
[svc_storage 写 TF 卡]
        │
        ├─→ fopen("/sdcard/DCIM/IMG_20260927_103000.jpg", "wb")
        ├─→ fwrite(jpeg_data, ...)
        └─→ fclose()
        │
        ▼
[periph_camera_release_frame(&frame)]
        │
        ▼
[svc_notification_post("已保存到 DCIM")]
```

## 10. 语音唤醒（Phase 3）

```
[mic_task 持续从 I2S 读取 PCM (16 kHz)]
        │
        ▼
[本地语音识别引擎处理]
        │
        ├─→ VAD 检测
        │
        └─→ 本地唤醒词模型
              │
              └─→ 检测到 "你好小智" 唤醒词
                    │
                    ▼
              [切换到命令词识别]
                    │
                    ├─→ "播放音乐" → svc_audio_play(...)
                    ├─→ "暂停" → svc_audio_pause()
                    ├─→ "下一首" → svc_audio_next()
                    └─→ "打开设置" → fw_app_mgr_launch("Settings")
```

## 11. 多任务切换

```
[用户从 App A 长按 HOME 键]
        │
        ▼
[fw_app_mgr 弹出最近应用列表]
        │
        ▼
[用户选择 App B]
        │
        ▼
[fw_app_mgr_back_to_app(B)]
        │
        ├─→ A: on_pause() （A 进入后台，资源保留）
        ├─→ B: on_resume() （B 从后台恢复）
        └─→ 显示切换动画
```

## 12. 性能预算汇总

| 场景 | 目标 |
|------|------|
| 开机到首屏 | < 2.5 s |
| App 切换 | < 500 ms |
| 触摸响应 | < 100 ms |
| Wi-Fi 连接（已知） | < 5 s |
| 拍照保存 | < 1 s |
| MP3 启动 | < 300 ms |
| UI 帧率 | ≥ 30 FPS |

## 13. 关键路径与瓶颈

### 13.1 关键路径（必须流畅）

- **触摸 → UI**：FT6336 → LVGL input device → 当前屏事件回调
- **音频播放**：play_task → 解码 → I2S → ES8311
- **显示刷新**：LVGL tick → render → LCD

### 13.2 潜在瓶颈

- **SRAM**：512 KB 内置，任务栈 + LVGL 控制块紧张
- **PSRAM**：8 MB，framebuffer 占大头
- **音频 + UI 同时**：核心 1 跑音频解码，核心 0 跑 LVGL，互不干扰

## 14. 数据流图（综合）

```
┌──────────────────────────────────────────────────────┐
│ 用户交互（触摸 / 按键 / IMU）                        │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Drivers（硬件读写）                                  │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Peripherals（业务语义封装）                          │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Services（事件总线 / 业务逻辑）                       │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Framework（LVGL 封装 / 窗口 / 应用管理）              │
└─────┬────────────────────────────────────────────────┘
      ▼
┌──────────────────────────────────────────────────────┐
│ Apps（具体业务：时钟 / 音乐 / 设置等）                │
└──────────────────────────────────────────────────────┘
```

每一层通过事件总线（svc_event_bus）跨层通信，避免直接跨层调用。