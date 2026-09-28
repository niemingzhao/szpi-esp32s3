# 系统服务层详细设计

Services 层提供跨多个 Peripherals 的业务服务，处理协议、状态、并发。本文档定义每个服务的接口、内部状态、任务模型、消息流。

## 1. 服务清单

| 服务 | 头文件 | 主要职责 |
|------|--------|----------|
| Event Bus | `svc_event_bus.h` | 系统事件分发 |
| Time | `svc_time.h` | SNTP 同步、时区 |
| Audio | `svc_audio.h` | 音乐 / 录音 / 提示音 |
| Network | `svc_net.h` | Wi-Fi 状态机、HTTP / MQTT / WebSocket |
| Storage | `svc_storage.h` | 文件系统抽象 |
| Notification | `svc_notification.h` | 通知队列、UI 路由 |
| Power | `svc_power.h` | 电源管理 |

## 2. svc_event_bus（事件总线）

### 2.1 职责

- 提供发布-订阅模型
- 在独立的 dispatcher 任务中调用订阅者回调（避免阻塞发布者）
- 支持从 ISR 发布

### 2.2 接口

```c
typedef enum {
    SVC_EVENT_BASE = 0,

    // 存储事件
    SVC_EVENT_SD_MOUNTED,
    SVC_EVENT_SD_UNMOUNTED,
    SVC_EVENT_SD_ERROR,

    // 网络事件
    SVC_EVENT_WIFI_SCAN_STARTED,
    SVC_EVENT_WIFI_SCAN_DONE,
    SVC_EVENT_WIFI_CONNECTING,
    SVC_EVENT_WIFI_CONNECTED,
    SVC_EVENT_WIFI_DISCONNECTED,
    SVC_EVENT_WIFI_CONNECT_FAILED,

    // 时间事件
    SVC_EVENT_TIME_SYNCED,
    SVC_EVENT_TIME_CHANGED,         // 每分钟
    SVC_EVENT_TIMEZONE_CHANGED,

    // 主题 / 语言事件
    SVC_EVENT_THEME_CHANGED,
    SVC_EVENT_LANGUAGE_CHANGED,

    // 电源 / 输入事件
    SVC_EVENT_BRIGHTNESS_CHANGED,
    SVC_EVENT_TOUCH,
    SVC_EVENT_SHUTDOWN_REQUEST,

    // 手势事件
    SVC_EVENT_GESTURE_SWIPE_LEFT,
    SVC_EVENT_GESTURE_SWIPE_RIGHT,
    SVC_EVENT_GESTURE_SWIPE_UP,
    SVC_EVENT_GESTURE_SWIPE_DOWN,

    // 音频事件
    SVC_EVENT_AUDIO_PLAYBACK_STARTED,
    SVC_EVENT_AUDIO_PLAYBACK_FINISHED,
    SVC_EVENT_AUDIO_PLAYBACK_ERROR,
    SVC_EVENT_AUDIO_RECORD_STARTED,
    SVC_EVENT_AUDIO_RECORD_FINISHED,

    // 通知事件
    SVC_EVENT_NOTIFICATION_POSTED,
    SVC_EVENT_NOTIFICATION_DISMISSED,
    SVC_EVENT_NOTIFICATION_CLICKED,

    // 用户事件
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

- 内部维护 `handler_table[event_id]` 链表
- 内部队列 `event_queue`（深度 32）
- 单一 `dispatcher_task` (优先级 5, 核心 0) 从队列取事件并调用所有订阅者
- 订阅者回调中禁止阻塞太久

## 3. svc_time（时间服务）

### 3.1 职责

- SNTP 同步
- 时区管理
- 提供格式化时间 API

### 3.2 接口

```c
esp_err_t svc_time_init(void);

esp_err_t svc_time_sync_ntp(void);
esp_err_t svc_time_set_timezone(const char *tz);  // "CST-8" 或 "Asia/Shanghai"

bool svc_time_is_synced(void);

/** 获取 epoch 秒 */
int64_t svc_time_now(void);

/** 格式化 */
esp_err_t svc_time_format(int64_t ts, const char *fmt, char *buf, size_t len);
// fmt 同 strftime: "%H:%M", "%Y-%m-%d %H:%M:%S", ...

esp_err_t svc_time_set_manual(int64_t ts);
```

### 3.3 实现要点

- `ntp_task` (优先级 2, 核心 0) 每 6 小时同步一次
- 同步成功后发布 `SVC_EVENT_TIME_SYNCED`
- 每分钟发送 `SVC_EVENT_TIME_CHANGED`
- 用 `esp_netif_sntp` + `esp_sntp`

## 4. svc_audio（音频服务）

### 4.1 职责

- 音乐播放：MP3 / WAV
- 录音：WAV 写入 TF 卡
- 提示音（tone）：短促蜂鸣
- 音量控制

### 4.2 接口

```c
typedef enum {
    SVC_AUDIO_SRC_FILE,     // 文件路径
    SVC_AUDIO_SRC_URL,      // HTTP URL
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

### 4.3 任务架构

```
                ┌────────────────┐
                │  API Call      │
                └───────┬────────┘
                        ▼
                ┌────────────────┐   play_task 优先级 6, 核心 1, 栈 8 KB
                │  play_task     │   解码 (helix MP3 / wav)
                │  状态机        │   写入 I2S
                └───────┬────────┘
                        ▼
                ┌────────────────┐   rec_task 优先级 5, 核心 1, 栈 4 KB
                │  rec_task      │   读取 I2S
                │  (录音时启用)  │   写 WAV 头 + PCM
                └───────┬────────┘
                        ▼
                ┌────────────────┐   tone_task 优先级 4, 核心 1, 栈 2 KB
                │  tone_task     │   短 tone 临时覆盖
                │  (提示音)      │
                └────────────────┘
```

### 4.4 实现要点

- 使用 esp-audio-player 作为高层播放 API
- 使用 helix MP3 解码器
- 状态通过互斥锁保护
- 异步回调通过 `xQueueSend` 投递到 `play_task` 处理
- tone 实现：用一个临时 PCM 流任务，频率控制由正弦波生成器

## 5. svc_net（网络服务）

### 5.1 职责

- Wi-Fi 状态机（连接、断开、重连）
- 蓝牙（BLE 主机 / 外设）
- HTTP 客户端
- MQTT 客户端
- WebSocket 客户端
- SNTP 时间同步（注册到 svc_time）
- OTA 升级

### 5.2 接口

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

esp_err_t svc_net_init(void);

/** 启动 Wi-Fi */
esp_err_t svc_net_wifi_start(svc_net_mode_t mode);
esp_err_t svc_net_wifi_stop(void);

/** 扫描 AP */
esp_err_t svc_net_wifi_scan(svc_net_wifi_ap_t *aps, size_t max_aps, size_t *found, uint32_t timeout_ms);

/** 连接（已知 SSID） */
esp_err_t svc_net_wifi_connect(const svc_net_wifi_creds_t *creds);
esp_err_t svc_net_wifi_disconnect(void);

/** 获取已保存的凭据并尝试自动连接 */
esp_err_t svc_net_wifi_auto_connect(void);

/** 清除已保存的凭据并断开 / 关闭 Wi-Fi */
esp_err_t svc_net_wifi_forget(void);

/** SmartConfig 配网 */
esp_err_t svc_net_smartconfig_start(void);
esp_err_t svc_net_smartconfig_stop(void);

/** 状态查询 */
typedef struct {
    bool wifi_connected;
    char wifi_ssid[33];
    char ip_addr[16];
    int8_t rssi;
} svc_net_status_t;
esp_err_t svc_net_get_status(svc_net_status_t *status);

/** HTTP 客户端 */
esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms);

/** MQTT */
esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password);
esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos);
esp_err_t svc_mqtt_subscribe(const char *topic, int qos, void (*cb)(const char *topic, const char *payload));
esp_err_t svc_mqtt_disconnect(void);

/** WebSocket */
esp_err_t svc_ws_connect(const char *uri, void (*cb)(const char *data, size_t len));
esp_err_t svc_ws_send(const char *data, size_t len);
esp_err_t svc_ws_disconnect(void);

/** OTA */
esp_err_t svc_ota_check_and_update(const char *url);
```

### 5.3 实现要点

- Wi-Fi / IP 事件由 `esp_event` 默认事件循环任务处理，回调中发布 `SVC_EVENT_WIFI_*` 事件
- 默认保存一份 Wi-Fi 凭据到 NVS，自动连接
- 连接失败自动重试（最多 5 次），超限后停止并发布 `SVC_EVENT_WIFI_CONNECT_FAILED`
- 开启 PMF capable（兼容 WPA3），`sae_pwe_h2e` 用 `WPA3_SAE_PWE_BOTH`
- HTTP 客户端用 `esp_http_client`
- MQTT 用 `mqtt`
- WebSocket 用 `esp_websocket_client`
- OTA 用 `esp_https_ota`，带 HTTPS 校验

## 6. svc_storage（存储服务）

### 6.1 职责

- 挂载 / 卸载 TF 卡 + 内置 Flash
- 提供统一路径管理
- 应用沙箱目录
- 文件浏览辅助

### 6.2 接口

```c
esp_err_t svc_storage_init(void);

esp_err_t svc_storage_get_path(periph_storage_type_t type, char *buf, size_t len);
// 填入 "/sdcard" 或 "/internal"

esp_err_t svc_storage_app_dir(const char *app_name, char *buf, size_t len);
// 返回 /internal/apps/<app_name>/

esp_err_t svc_storage_mkdir(const char *path);
esp_err_t svc_storage_rmdir(const char *path);

/** 列出目录内容（迭代器） */
typedef struct {
    char name[256];
    bool is_dir;
    size_t size;
} svc_storage_entry_t;

typedef void *svc_storage_iter_t;
esp_err_t svc_storage_iter_start(const char *dir, svc_storage_iter_t *iter);
svc_storage_entry_t *svc_storage_iter_next(svc_storage_iter_t iter);
void svc_storage_iter_end(svc_storage_iter_t iter);
```

## 7. svc_notification（通知服务）

### 7.1 职责

- 通知队列
- 通知显示（通过 framework 注册）
- 通知点击回调

### 7.2 接口

```c
typedef enum {
    SVC_NOTI_TYPE_INFO,
    SVC_NOTI_TYPE_WARN,
    SVC_NOTI_TYPE_ERROR,
    SVC_NOTI_TYPE_SUCCESS,
    SVC_NOTI_TYPE_PROGRESS,
} svc_noti_type_t;

typedef void (*svc_noti_click_cb_t)(uint32_t noti_id, void *user_data);

typedef struct {
    uint32_t id;
    svc_noti_type_t type;
    const char *icon_src;
    const char *title;
    const char *message;
    uint32_t timestamp;
    bool auto_dismiss_ms;
    void *user_data;
    svc_noti_click_cb_t on_click;
} svc_notification_t;

esp_err_t svc_notification_init(void);

esp_err_t svc_notification_post(const svc_notification_t *noti);
esp_err_t svc_notification_dismiss(uint32_t noti_id);
esp_err_t svc_notification_clear_all(void);

size_t svc_notification_get_count(void);
esp_err_t svc_notification_get(size_t index, svc_notification_t *out);
```

### 7.3 实现要点

- 通知存储为链表 / 动态数组
- 每次 post / dismiss 发送 `SVC_EVENT_NOTIFICATION_POSTED / DISMISSED`
- Framework 监听这些事件并更新 UI

## 8. svc_power（电源服务）

### 8.1 职责

- 背光超时熄屏
- 触摸唤醒
- 深度睡眠（预留）
- 关机 / 重启请求

### 8.2 接口

```c
esp_err_t svc_power_init(void);

esp_err_t svc_power_set_backlight_timeout(uint32_t seconds);  // 0 = 不超时
uint32_t svc_power_get_backlight_timeout(void);

esp_err_t svc_power_set_brightness(uint8_t percent);  // 0-100，发布 SVC_EVENT_BRIGHTNESS_CHANGED
uint8_t svc_power_get_brightness(void);

esp_err_t svc_power_wake(void);
esp_err_t svc_power_sleep(void);
bool svc_power_is_sleeping(void);

esp_err_t svc_power_request_reboot(void);
esp_err_t svc_power_request_shutdown(void);
```

### 8.3 实现要点

- `power_task` (优先级 2, 核心 0) 每秒检查
- 触摸事件 (`SVC_EVENT_TOUCH` 通过事件总线) 重置超时
- 同时把触摸 / 手势事件转成系统事件：`SVC_EVENT_TOUCH`、`SVC_EVENT_GESTURE_SWIPE_*`
- 关机请求：发布 `SVC_EVENT_SHUTDOWN_REQUEST`，主循环收到后 `esp_restart()`
- 本板无电池，`shutdown` 用 `deep sleep` 模拟

## 9. 服务初始化顺序

```c
esp_err_t services_init(void) {
    ESP_ERROR_CHECK(svc_event_bus_init());        // 1. 第一个，所有服务都依赖
    ESP_ERROR_CHECK(svc_storage_init());          // 2. 存储（其他服务可能要用）
    ESP_ERROR_CHECK(svc_time_init());             // 3. 时间
    ESP_ERROR_CHECK(svc_audio_init());            // 4. 音频
    ESP_ERROR_CHECK(svc_net_init());              // 5. 网络
    ESP_ERROR_CHECK(svc_power_init());            // 6. 电源
    ESP_ERROR_CHECK(svc_notification_init());     // 7. 通知（依赖事件总线）
    return ESP_OK;
}
```

## 10. 任务模型

每个服务使用一个独立 FreeRTOS 任务：

| 服务 | 任务名 | 优先级 | 核心 |
|------|--------|--------|------|
| Event Bus | dispatcher_task | 5 | 0 |
| Time | ntp_sync_task | 2 | 0 |
| Audio | play_task | 6 | 1 |
| Audio | rec_task | 5 | 1 |
| Audio | tone_task | 4 | 1 |
| Network | net_event_task | 4 | 0 |
| Power | power_task | 2 | 0 |

## 11. 跨服务依赖

- `svc_audio` → `svc_storage`（路径 / 目录；文件内容经 VFS 直接读取）
- `svc_audio` → `svc_notification`（通知点击后播放提示音）
- `svc_net` → `svc_time`（SNTP 注册）
- `svc_notification` → 所有服务（订阅 `*_POSTED` / `*_FINISHED` 事件）
- `svc_power` → 所有服务（订阅 `SVC_EVENT_TOUCH` 事件）
