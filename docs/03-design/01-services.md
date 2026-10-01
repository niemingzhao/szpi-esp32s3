# 系统服务层详细设计

Services 层提供跨多个 Peripherals 的业务服务，处理协议、状态、并发。本文档定义每个服务的接口、内部状态、任务模型、消息流。

## 1. 服务清单

| 服务 | 头文件 | 主要职责 |
|------|--------|----------|
| Event Bus | `svc_event_bus.h` | 系统事件分发 |
| Settings | `svc_settings.h` | 配置持久化（NVS，含 blob） |
| Storage | `svc_storage.h` | 文件系统抽象、TF 热插拔 |
| Time | `svc_time.h` | 时间、时区、SNTP |
| Audio | `svc_audio.h` | 音乐 / 录音 / 提示音 |
| Network | `svc_net.h` | Wi-Fi 状态机、HTTP(S)、配网 |
| Bluetooth | `svc_bt.h` | BLE 广播、扫描、GATT 从机、HID 模拟 |
| MQTT | `svc_mqtt.h` | MQTT 客户端 |
| WebSocket | `svc_ws.h` | WebSocket 客户端 |
| IO | `svc_io.h` | 外扩 GPIO / PWM / I2C / UART / ADC |
| Camera | `svc_camera.h` | 摄像头（把 esp32-camera 挡在服务内） |
| Power | `svc_power.h` | 背光、熄屏、唤醒、关机 / 重启 |
| IMU | `svc_imu.h` | 姿态采样与运动事件 |
| Watchdog | `svc_watchdog.h` | Task Watchdog |
| System Info | `svc_sysinfo.h` | 系统信息、崩溃记录 |

## 2. svc_event_bus（事件总线）

### 2.1 职责

- 提供发布-订阅模型
- 在独立的 dispatcher 任务中调用订阅者回调（避免阻塞发布者）
- 支持从 ISR 发布

### 2.2 接口

```c
typedef enum {
    SVC_EVENT_BASE = 0,

    // 存储
    SVC_EVENT_SD_MOUNTED,
    SVC_EVENT_SD_UNMOUNTED,

    // 网络
    SVC_EVENT_WIFI_CONNECTING,
    SVC_EVENT_WIFI_CONNECTED,
    SVC_EVENT_WIFI_DISCONNECTED,
    SVC_EVENT_WIFI_CONNECT_FAILED,

    // 蓝牙
    SVC_EVENT_BT_STATE_CHANGED,
    SVC_EVENT_BT_SCAN_DONE,

    // 时间
    SVC_EVENT_TIME_SYNCED,
    SVC_EVENT_TIME_CHANGED,
    SVC_EVENT_TIMEZONE_CHANGED,

    // 主题
    SVC_EVENT_THEME_CHANGED,

    // 电源
    SVC_EVENT_BRIGHTNESS_CHANGED,
    SVC_EVENT_TOUCH,

    // IMU
    SVC_EVENT_IMU_MOTION,
    SVC_EVENT_IMU_ORIENTATION,

    // 音频
    SVC_EVENT_AUDIO_PLAYBACK_STARTED,
    SVC_EVENT_AUDIO_PLAYBACK_FINISHED,
    SVC_EVENT_AUDIO_PLAYBACK_ERROR,
    SVC_EVENT_AUDIO_RECORD_STARTED,
    SVC_EVENT_AUDIO_RECORD_FINISHED,

    // 脚本
    SVC_EVENT_SCRIPT_STARTED,
    SVC_EVENT_SCRIPT_STOPPED,

    SVC_EVENT_USER_BASE = 0x8000,
} svc_event_id_t;

typedef struct {
    svc_event_id_t id;
    void *data;             // 发送方提供，订阅方只读
    uint32_t data_len;
} svc_event_t;

typedef void (*svc_event_handler_t)(const svc_event_t *evt, void *user_data);

esp_err_t svc_event_bus_init(void);
esp_err_t svc_event_bus_subscribe(svc_event_id_t id, svc_event_handler_t h, void *user_data);
esp_err_t svc_event_bus_unsubscribe(svc_event_id_t id, svc_event_handler_t h);
esp_err_t svc_event_bus_publish(svc_event_id_t id, void *data, uint32_t len);
esp_err_t svc_event_bus_publish_from_isr(svc_event_id_t id, void *data, uint32_t len);
```

### 2.3 实现要点

- 订阅表是定长数组（全局上限 `SVC_EVENT_MAX_SUBS` = 32），同一 (id, handler, user_data) 重复订阅幂等
- 事件经内部队列投递给单一 dispatcher 任务；队列满时记日志并返回 `ESP_ERR_TIMEOUT`（不静默丢）
- 单条事件负载超过 `SVC_EVENT_DATA_MAX` 会截断并记日志
- 订阅者回调里不要长时间阻塞；订阅 / 退订只在初始化或 App 生命周期回调里做

## 3. svc_settings（配置服务）

```c
esp_err_t svc_settings_init(void);
esp_err_t svc_settings_set_i32(const char *ns, const char *key, int32_t val);
esp_err_t svc_settings_get_i32(const char *ns, const char *key, int32_t *val, int32_t def);
esp_err_t svc_settings_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t svc_settings_get_u32(const char *ns, const char *key, uint32_t *val, uint32_t def);
esp_err_t svc_settings_set_str(const char *ns, const char *key, const char *val);
esp_err_t svc_settings_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def);
esp_err_t svc_settings_set_blob(const char *ns, const char *key, const void *data, size_t len);
esp_err_t svc_settings_get_blob(const char *ns, const char *key, void *buf, size_t *len);
```

- 命名空间：`sys`、`wifi`、`script`、`app_<name>`
- 设置变化发布对应的 `SVC_EVENT_*_CHANGED` 事件（如亮度、时区、主题）
- 所有读写走 NVS，进程内不缓存

## 4. svc_storage（存储服务）

```c
esp_err_t svc_storage_init(void);

esp_err_t svc_storage_get_path(periph_storage_type_t type, char *buf, size_t len);
// 填入 "/sdcard" 或 "/internal"

esp_err_t svc_storage_app_dir(const char *app_name, char *buf, size_t len);
// 返回 /internal/apps/<app_name>（不带结尾斜杠）

esp_err_t svc_storage_mkdir(const char *path);
esp_err_t svc_storage_rmdir(const char *path);

/* 整块读写（读上限 1 MB，返回的缓冲由调用方 free） */
esp_err_t svc_storage_read(const char *path, void **out_buf, size_t *out_len);
esp_err_t svc_storage_write(const char *path, const void *data, size_t len);
esp_err_t svc_storage_remove(const char *path);
esp_err_t svc_storage_exists(const char *path, size_t *out_size);

/* 目录迭代：iter_next 返回内部缓冲，用完必须马上拷贝 */
typedef struct {
    char name[256];
    bool is_dir;
    size_t size;
} svc_storage_entry_t;
typedef void *svc_storage_iter_t;
esp_err_t svc_storage_iter_start(const char *dir, svc_storage_iter_t *iter);
svc_storage_entry_t *svc_storage_iter_next(svc_storage_iter_t iter);
void svc_storage_iter_end(svc_storage_iter_t iter);

esp_err_t svc_storage_format(periph_storage_type_t type);
```

- 挂载 / 卸载与热插拔委托 `periph_storage`，本层负责路径与 App 目录约定
- TF 卡热插拔由 `sd_monitor_task` 检测，挂载 / 卸载后发布 `SVC_EVENT_SD_*`

## 5. svc_time（时间服务）

```c
esp_err_t svc_time_init(void);

esp_err_t svc_time_sync_ntp(void);
esp_err_t svc_time_set_timezone(const char *tz);   // "CST-8"
esp_err_t svc_time_set_manual(int64_t ts);

bool svc_time_is_synced(void);
int64_t svc_time_now(void);                        // epoch 秒

esp_err_t svc_time_format(int64_t ts, const char *fmt, char *buf, size_t len);
// fmt 同 strftime："%H:%M"、"%-m月%-d日" 等
```

- `ntp_sync_task`（优先级 2，核心 0）每 6 小时同步一次，成功后发布 `SVC_EVENT_TIME_SYNCED`
- 每分钟发布 `SVC_EVENT_TIME_CHANGED`
- 用 `esp_netif_sntp` + `esp_sntp`

## 6. svc_audio（音频服务）

### 6.1 职责

- 音乐播放：MP3 / WAV
- 录音：WAV 写入 TF 卡
- 提示音（tone）：短促蜂鸣
- 音量与静音

### 6.2 接口

```c
typedef enum {
    SVC_AUDIO_SRC_FILE,     // 文件路径
    SVC_AUDIO_SRC_TONE,     // 简单 tone
    SVC_AUDIO_SRC_STREAM,   // 外部 PCM 流
} svc_audio_src_type_t;

typedef enum {
    SVC_AUDIO_STATE_IDLE,
    SVC_AUDIO_STATE_PLAYING,
    SVC_AUDIO_STATE_PAUSED,
    SVC_AUDIO_STATE_RECORDING,
} svc_audio_state_t;

typedef struct {
    svc_audio_src_type_t type;
    const char *uri;
    uint16_t tone_freq;
    uint32_t tone_duration_ms;
    bool loop;
} svc_audio_source_t;

typedef enum {
    SVC_AUDIO_EVT_STATE_CHANGED,
    SVC_AUDIO_EVT_PLAYBACK_FINISHED,
    SVC_AUDIO_EVT_PLAYBACK_ERROR,
    SVC_AUDIO_EVT_RECORD_FINISHED,
} svc_audio_evt_t;

typedef struct {
    svc_audio_evt_t evt;
    union {
        svc_audio_state_t new_state;
        esp_err_t error;
    };
} svc_audio_evt_data_t;

typedef void (*svc_audio_cb_t)(const svc_audio_evt_data_t *evt, void *user);

esp_err_t svc_audio_init(void);
esp_err_t svc_audio_deinit(void);

esp_err_t svc_audio_play(const svc_audio_source_t *src);
esp_err_t svc_audio_pause(void);
esp_err_t svc_audio_resume(void);
esp_err_t svc_audio_stop(void);

esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds);
esp_err_t svc_audio_record_stop(void);

esp_err_t svc_audio_set_volume(uint8_t percent);
uint8_t svc_audio_get_volume(void);
esp_err_t svc_audio_set_mute(bool mute);

svc_audio_state_t svc_audio_get_state(void);

esp_err_t svc_audio_register_callback(svc_audio_cb_t cb, void *user);

/** 异步播放 tone（不打断当前音频） */
esp_err_t svc_audio_play_tone_async(uint16_t freq_hz, uint32_t ms);
```

### 6.3 任务架构

```
                ┌────────────────┐
                │  API Call      │
                └───────┬────────┘
                        ▼
                ┌────────────────┐   play_task 优先级 6, 核心 1, 栈 8 KB
                │  play_task     │   解码 (helix MP3 / WAV)
                │  状态机        │   写入 I2S
                └───────┬────────┘
                        ▼
                ┌────────────────┐   rec_task（录音时启用）
                │  rec_task      │   读取 I2S
                │                │   写 WAV 头 + PCM
                └───────┬────────┘
                        ▼
                ┌────────────────┐   tone_task
                │  tone_task     │   短 tone 临时覆盖
                │  (提示音)      │
                └────────────────┘
```

### 6.4 实现要点

- MP3 用 helix 解码器；WAV 直接读 PCM
- 文件读取经 `svc_storage` 的路径，数据经 VFS 直接读
- 播放状态由互斥锁保护，事件回调经队列投递到 play_task
- tone 用正弦波生成器临时占用播放通路，播完恢复

## 7. svc_net（网络服务）

### 7.1 职责

- Wi-Fi 状态机（扫描、连接、断开、重连）
- Web 配网 / SmartConfig
- HTTP(S) 客户端
- SNTP 时间同步（委托 `svc_time`）

蓝牙 / MQTT / WebSocket 分别由 `svc_bt` / `svc_mqtt` / `svc_ws` 提供。

### 7.2 接口

```c
typedef enum {
    SVC_NET_MODE_OFF,
    SVC_NET_MODE_STA,
    SVC_NET_MODE_AP,
    SVC_NET_MODE_STA_AP,
} svc_net_mode_t;

typedef struct {
    char ssid[33];
    char password[64];
} svc_net_wifi_creds_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_mode;
} svc_net_wifi_ap_t;

typedef struct {
    bool wifi_connected;
    char wifi_ssid[33];
    char ip_addr[16];
    int8_t rssi;
} svc_net_status_t;

esp_err_t svc_net_init(void);

esp_err_t svc_net_wifi_start(svc_net_mode_t mode);
esp_err_t svc_net_wifi_stop(void);
esp_err_t svc_net_wifi_scan(svc_net_wifi_ap_t *aps, size_t max_aps, size_t *found, uint32_t timeout_ms);
esp_err_t svc_net_wifi_connect(const svc_net_wifi_creds_t *creds);
esp_err_t svc_net_wifi_disconnect(void);
esp_err_t svc_net_wifi_auto_connect(void);
esp_err_t svc_net_wifi_forget(void);
esp_err_t svc_net_wifi_get_saved_ssid(char *buf, size_t len);
esp_err_t svc_net_get_status(svc_net_status_t *status);

esp_err_t svc_net_smartconfig_start(void);
esp_err_t svc_net_smartconfig_stop(void);

esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
```

### 7.3 实现要点

- Wi-Fi / IP 事件由 `esp_event` 默认事件循环任务处理，回调中发布 `SVC_EVENT_WIFI_*`
- 保存一份凭据到 NVS，支持自动重连；连接失败重试若干次后停止并发布 `SVC_EVENT_WIFI_CONNECT_FAILED`
- 开启 PMF capable（兼容 WPA3）
- HTTP 客户端用 `esp_http_client`，HTTPS 走 mbedTLS

## 8. svc_bt（蓝牙服务）

```c
typedef enum {
    SVC_BT_STATE_OFF,
    SVC_BT_STATE_READY,
    SVC_BT_STATE_ADVERTISING,
    SVC_BT_STATE_CONNECTED,
} svc_bt_state_t;

esp_err_t svc_bt_init(void);          // 必须在 Wi-Fi 之前初始化（AGENTS 4.6）
svc_bt_state_t svc_bt_get_state(void);
esp_err_t svc_bt_adv_start(void);
esp_err_t svc_bt_adv_stop(void);
esp_err_t svc_bt_scan_start(void);
esp_err_t svc_bt_scan_stop(void);
esp_err_t svc_bt_hid_send_key(uint8_t keycode);
esp_err_t svc_bt_hid_send_mouse(int8_t dx, int8_t dy, uint8_t buttons);
```

- 仅 BLE 4.2，Bluedroid；控制器活动实例数保持 IDF 默认
- 事件发布 `SVC_EVENT_BT_STATE_CHANGED` / `SVC_EVENT_BT_SCAN_DONE`
- HID 报告要等配对完成后才能发

## 9. svc_mqtt / svc_ws / svc_io / svc_camera

```c
/* svc_mqtt */
esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password);
esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos);
esp_err_t svc_mqtt_subscribe(const char *topic, int qos, void (*cb)(const char *topic, const char *payload));
esp_err_t svc_mqtt_disconnect(void);

/* svc_ws */
esp_err_t svc_ws_connect(const char *uri, void (*cb)(const char *data, size_t len));
esp_err_t svc_ws_send(const char *data, size_t len);
esp_err_t svc_ws_disconnect(void);

/* svc_io：外扩硬件能力，供脚本与 App 使用 */
esp_err_t svc_io_gpio_write(uint8_t gpio, uint8_t level);
int svc_io_gpio_read(uint8_t gpio);
esp_err_t svc_io_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent);
esp_err_t svc_io_adc_read(uint8_t gpio, int *out_mv);
esp_err_t svc_io_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t svc_io_i2c_read(uint8_t addr, uint8_t *data, size_t len);
esp_err_t svc_io_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);
esp_err_t svc_io_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t svc_io_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms);

/* svc_camera：封装 esp32-camera，App / 脚本不直接依赖该组件 */
esp_err_t svc_camera_open(void);
esp_err_t svc_camera_close(void);
bool svc_camera_is_open(void);
esp_err_t svc_camera_capture(void **out_buf, size_t *out_len, uint16_t *w, uint16_t *h, bool jpeg);
void svc_camera_release(void *buf);
```

- `svc_io` 与 `svc_camera` 是薄封装，直接转发到 `periph_ext` / `periph_camera`
- MQTT / WebSocket 使用各自托管组件

## 10. svc_power（电源服务）

```c
esp_err_t svc_power_init(void);

esp_err_t svc_power_set_backlight_timeout(uint32_t seconds);  // 0 = 不超时
uint32_t svc_power_get_backlight_timeout(void);

esp_err_t svc_power_set_brightness(uint8_t percent);          // 0-100，发布 SVC_EVENT_BRIGHTNESS_CHANGED
uint8_t svc_power_get_brightness(void);

esp_err_t svc_power_wake(void);
esp_err_t svc_power_sleep(void);
bool svc_power_is_sleeping(void);

esp_err_t svc_power_request_reboot(void);
esp_err_t svc_power_request_shutdown(void);
```

- `power_task`（优先级 2，核心 0）每秒检查；触摸 / 姿态事件重置超时
- 熄屏只关背光，LCD 与 LVGL 保持运行
- 关机 / 重启：重启走软复位；关机为屏幕关闭 + 待机（本板无电池，不做真正断电）

## 11. svc_imu（IMU 服务）

```c
typedef enum {
    SVC_IMU_EVT_ORIENTATION,   // 姿态变化
    SVC_IMU_EVT_SHAKE,         // 摇晃
    SVC_IMU_EVT_PICKUP,        // 抬手
} svc_imu_evt_t;

typedef void (*svc_imu_cb_t)(svc_imu_evt_t evt, void *user);

esp_err_t svc_imu_init(void);
esp_err_t svc_imu_read(periph_imu_data_t *out);
esp_err_t svc_imu_register_callback(svc_imu_cb_t cb, void *user);
```

- `imu_task`（优先级 3，核心 0，50 ms 周期）采样
- 判定姿态变化 / 摇晃 / 抬手，发布 `SVC_EVENT_IMU_MOTION` / `SVC_EVENT_IMU_ORIENTATION`
- 熄屏时用于姿态唤醒

## 12. 服务初始化顺序

```c
esp_err_t services_init(void) {
    ESP_ERROR_CHECK(svc_watchdog_init());     // 1. 看门狗（先配成"只告警"）
    ESP_ERROR_CHECK(svc_event_bus_init());    // 2. 事件总线（所有服务都依赖）
    ESP_ERROR_CHECK(svc_settings_init());     // 3. 配置（NVS）
    ESP_ERROR_CHECK(svc_storage_init());      // 4. 存储
    svc_bt_init();                            // 5. 蓝牙（必须在 Wi-Fi 之前，见 AGENTS 4.6）
    ESP_ERROR_CHECK(svc_time_init());         // 6. 时间
    ESP_ERROR_CHECK(svc_audio_init());        // 7. 音频
    ESP_ERROR_CHECK(svc_net_init());          // 8. 网络
    ESP_ERROR_CHECK(svc_power_init());        // 9. 电源
    ESP_ERROR_CHECK(svc_imu_init());          // 10. IMU
    ESP_ERROR_CHECK(svc_io_init());           // 11. 外扩 IO
    ESP_ERROR_CHECK(svc_camera_init());       // 12. 摄像头（不立即开）
    ESP_ERROR_CHECK(svc_sysinfo_init());      // 13. 系统信息（含崩溃记录）
    return ESP_OK;
}
```

`svc_bt_init()` 必须紧跟 Storage 之后、Wi-Fi 之前：控制器要一块连续内部内存，主机的工作队列与任务栈又只能用内部 RAM，晚于 Wi-Fi / LVGL / 音频就会随机初始化失败（细节见 `AGENTS.md` 4.6）。它失败只打警告，不阻塞启动。

`svc_watchdog_arm()` 不在 `services_init()` 里：要等启动全部完成（含首次 SPIFFS 格式化）后才打开"超时自动重启"，由 `main.c` 在挂载内置 Flash 之后调用。

## 13. 任务模型

| 服务 | 任务名 | 优先级 | 核心 |
|------|--------|--------|------|
| Event Bus | `event_bus_task` | 4 | 0 |
| Time | `ntp_sync_task` | 2 | 0 |
| Audio | `play_task` | 6 | 1 |
| Audio | `rec_task` | 5 | 1 |
| Audio | `tone_task` | 4 | 1 |
| Network | `net_event_task` | 4 | 0 |
| Power | `power_task` | 2 | 0 |
| IMU | `imu_task` | 3 | 0 |
| Storage | `sd_monitor_task` | 3 | 0 |

## 14. 跨服务依赖

- `svc_audio` → `svc_storage`（路径；文件内容经 VFS 直接读）
- `svc_net` → `svc_time`（SNTP 注册）
- `svc_io` → `periph_ext`
- `svc_camera` → `periph_camera`
- `svc_power` → 事件总线（订阅 `SVC_EVENT_TOUCH`、`SVC_EVENT_IMU_*`）
