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
    SVC_EVENT_SD_ERROR,

    // 网络
    SVC_EVENT_WIFI_SCAN_STARTED,
    SVC_EVENT_WIFI_SCAN_DONE,
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

    // 主题 / 语言
    SVC_EVENT_THEME_CHANGED,
    SVC_EVENT_LANGUAGE_CHANGED,

    // 电源
    SVC_EVENT_BRIGHTNESS_CHANGED,
    SVC_EVENT_TOUCH,
    // BOOT 键（负载为 uint8_t 事件序号：0 单击 / 1 双击 / 2 长按 / 3 极长按）
    SVC_EVENT_KEY,
    SVC_EVENT_SHUTDOWN_REQUEST,

    // IMU
    SVC_EVENT_IMU_MOTION,
    SVC_EVENT_IMU_ORIENTATION,
    SVC_EVENT_IMU_SHAKE,
    SVC_EVENT_IMU_PICKUP,

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
esp_err_t svc_event_bus_publish(svc_event_id_t id, const void *data, uint32_t len);
esp_err_t svc_event_bus_publish_from_isr(svc_event_id_t id, const void *data, uint32_t len);
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
esp_err_t svc_settings_set_u8(const char *ns, const char *key, uint8_t val);
esp_err_t svc_settings_get_u8(const char *ns, const char *key, uint8_t *val, uint8_t def);
esp_err_t svc_settings_set_str(const char *ns, const char *key, const char *val);
esp_err_t svc_settings_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def);
esp_err_t svc_settings_set_blob(const char *ns, const char *key, const void *data, size_t len);
esp_err_t svc_settings_get_blob(const char *ns, const char *key, void *buf, size_t *len);

esp_err_t svc_settings_factory_reset(void);
```

- 命名空间：`sys`、`wifi`、`script`、App 私有（如 App 天气用 `weather`）
- 设置变化发布对应的 `SVC_EVENT_*_CHANGED` 事件（如亮度、时区、主题）
- 所有读写走 NVS，进程内不缓存
- `svc_settings_factory_reset()` 擦除整个 NVS 分区，调用后应立即重启

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

/* 容量查询 */
esp_err_t svc_storage_get_info(periph_storage_type_t type, periph_storage_info_t *out);

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

- 音乐播放：MP3 / WAV，支持本地文件、HTTP(S) 链接与外部 PCM 流
- 录音：WAV 写入 TF 卡
- 提示音（tone）：短促蜂鸣
- 音量与静音

### 6.2 接口

```c
typedef enum {
    SVC_AUDIO_SRC_FILE,     // 文件路径（支持）
    SVC_AUDIO_SRC_URL,      // HTTP(S) 链接（支持，边下边播）
    SVC_AUDIO_SRC_TONE,     // 简单 tone
    SVC_AUDIO_SRC_STREAM,   // 外部 PCM 流（支持）
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
                ┌────────────────┐   svc_audio 优先级 6, 核心 1, 栈 4 KB
                │  svc_audio     │   播放：解码 (helix MP3 / WAV) → I2S
                │  状态机        │   录音：I2S → WAV 头 + PCM
                └────────────────┘   提示音：正弦波临时占用播放通路
```

播放 / 录音 / 提示音共用一条 I2S0（MCLK / BCLK / WS 是同一组引脚，MCLK = 采样率 × 256），
所以采样率与时钟由服务统一维护，只有一个音频任务串行访问 I2S。

### 6.4 实现要点

- MP3 用 helix 解码器；WAV 直接读 PCM
- 文件读取经 `svc_storage` 的路径，数据经 VFS 直接读
- 文件 / HTTP(S) 链接 / 外部 PCM 流三种播放来源共用同一播放通路：链接由 HTTP 客户端边下边播，外部流由调用方提供 PCM 数据
- 播放状态由互斥锁保护，事件回调经队列投递到 `svc_audio`
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

esp_err_t svc_net_prov_start(const char *ap_ssid, const char *ap_password);
esp_err_t svc_net_prov_stop(void);
bool svc_net_prov_is_active(void);

typedef void (*svc_http_cb_t)(const char *body, esp_err_t err, void *user);

esp_err_t svc_http_get(const char *url, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
esp_err_t svc_http_post(const char *url, const char *body, char *resp_buf, size_t buf_len, uint32_t timeout_ms);
esp_err_t svc_http_get_async(const char *url, char *resp_buf, size_t buf_len,
                             svc_http_cb_t cb, void *user);
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

typedef struct {
    uint8_t bda[6];
    int8_t rssi;
    char name[SVC_BT_NAME_MAX];
} svc_bt_scan_result_t;

typedef void (*svc_bt_data_cb_t)(const uint8_t *data, size_t len, void *user);

/* 诊断信息（排查蓝牙问题时用） */
typedef struct {
    svc_bt_state_t state;
    bool connected;
    bool adv_want;
    bool adv_ready;
    bool adv_active;
    bool scan_ready;
    uint32_t gap_events;
    uint32_t gap_last;
} svc_bt_diag_t;

esp_err_t svc_bt_init(void);          // 必须在 Wi-Fi 之前初始化（AGENTS 4.6）
esp_err_t svc_bt_deinit(void);        // 仅关机前释放用
svc_bt_state_t svc_bt_get_state(void);
const char *svc_bt_state_name(svc_bt_state_t state);
bool svc_bt_is_connected(void);
esp_err_t svc_bt_get_diag(svc_bt_diag_t *out);   // 诊断：状态 / 广播 / 扫描 / GAP 事件计数
esp_err_t svc_bt_adv_start(void);
esp_err_t svc_bt_adv_stop(void);
esp_err_t svc_bt_scan_start(uint32_t duration_s);   // 0 = 默认 10 s
esp_err_t svc_bt_scan_stop(void);
esp_err_t svc_bt_get_scan_results(svc_bt_scan_result_t *out, size_t max, size_t *count);
esp_err_t svc_bt_notify(const void *data, size_t len);
esp_err_t svc_bt_register_data_cb(svc_bt_data_cb_t cb, void *user);   // 注销传 NULL

/* svc_bt_hid.h：BLE HID 设备（键盘 / 鼠标 / 消费类控制） */
esp_err_t svc_bt_hid_init(void);
esp_err_t svc_bt_hid_deinit(void);
bool svc_bt_hid_is_ready(void);
esp_err_t svc_bt_hid_key(uint8_t usage, uint8_t modifier);
esp_err_t svc_bt_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy);
esp_err_t svc_bt_hid_consumer(uint8_t usage, bool pressed);
esp_err_t svc_bt_hid_key_click(uint8_t usage, uint8_t modifier);
esp_err_t svc_bt_hid_consumer_click(uint8_t usage);
```

- 仅 BLE 4.2，Bluedroid；控制器活动实例数保持 IDF 默认
- 仅作外设（peripheral）角色：广播、被中心设备连接、GATT 从机读写与通知；不发起中心设备连接，不做 GATT 客户端读写
- 事件发布 `SVC_EVENT_BT_STATE_CHANGED` / `SVC_EVENT_BT_SCAN_DONE`
- HID 报告要等配对完成后才能发（`svc_bt_hid_is_ready()`）
- GATTS / GAP 回调由 `svc_bt` 统一注册，再按 app_id / gatts_if 转发给 HID

## 9. svc_mqtt / svc_ws / svc_io / svc_camera

```c
/* svc_mqtt */
typedef void (*svc_mqtt_msg_cb_t)(const char *topic, const char *payload, size_t len, void *user);

esp_err_t svc_mqtt_connect(const char *uri, const char *username, const char *password);
esp_err_t svc_mqtt_publish(const char *topic, const char *payload, int qos);
esp_err_t svc_mqtt_subscribe(const char *topic, int qos, svc_mqtt_msg_cb_t cb, void *user);
esp_err_t svc_mqtt_unsubscribe(const char *topic);
bool svc_mqtt_is_connected(void);
esp_err_t svc_mqtt_disconnect(void);

/* svc_ws：仅文本帧 */
typedef void (*svc_ws_msg_cb_t)(const char *data, size_t len, void *user);

esp_err_t svc_ws_connect(const char *uri, svc_ws_msg_cb_t cb, void *user);
esp_err_t svc_ws_send(const char *data, size_t len);   /* 发送文本帧 */
bool svc_ws_is_connected(void);
esp_err_t svc_ws_disconnect(void);

/* svc_io：外扩硬件能力，供脚本与 App 使用 */
esp_err_t svc_io_gpio_write(uint8_t gpio, uint8_t level);
int svc_io_gpio_read(uint8_t gpio);
esp_err_t svc_io_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent);
esp_err_t svc_io_pwm_stop(uint8_t gpio);
esp_err_t svc_io_adc_read(uint8_t gpio, int *out_mv);
esp_err_t svc_io_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t svc_io_i2c_read(uint8_t addr, uint8_t *data, size_t len);
esp_err_t svc_io_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);
esp_err_t svc_io_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t svc_io_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms);

/* svc_camera：封装 esp32-camera，App / 脚本不直接依赖该组件 */
esp_err_t svc_camera_init(void);                  /* 服务初始化，不打开硬件 */
esp_err_t svc_camera_open(svc_camera_format_t fmt, svc_camera_size_t size);
esp_err_t svc_camera_close(void);
bool svc_camera_is_open(void);
esp_err_t svc_camera_capture(svc_camera_frame_t *out);
void svc_camera_release(void);
```

- `svc_io` 与 `svc_camera` 是薄封装，直接转发到 `periph_ext` / `periph_camera`
- `svc_ws` 只收发文本帧，不做二进制帧
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
    SVC_IMU_ORIENTATION_PORTRAIT,
    SVC_IMU_ORIENTATION_LANDSCAPE,
    SVC_IMU_ORIENTATION_PORTRAIT_FLIP,
    SVC_IMU_ORIENTATION_LANDSCAPE_FLIP,
} svc_imu_orientation_t;

esp_err_t svc_imu_init(void);
bool svc_imu_is_moving(void);
svc_imu_orientation_t svc_imu_get_orientation(void);
esp_err_t svc_imu_read(periph_imu_data_t *out);
```

- `imu_task`（优先级 3，核心 0，50 ms 周期）采样
- 判定运动 / 朝向变化，发布 `SVC_EVENT_IMU_MOTION` / `SVC_EVENT_IMU_ORIENTATION`；判定摇晃与抬手，发布 `SVC_EVENT_IMU_SHAKE` / `SVC_EVENT_IMU_PICKUP`
- 阈值可用 `svc_settings` 的 `imu` 命名空间调整，见下表；服务启动时读一次并缓存，运行期改 NVS 需重启服务（或重启设备）才生效
- 熄屏时用于姿态唤醒

阈值 key（比例类按 100 倍整数存）：

| key | 默认 | 语义 |
|-----|------|------|
| `motion_acc_pct` | 15 | 合加速度偏离 1 g 的百分比（存 15 = 15%） |
| `motion_gyro_dps` | 60 | 角速度阈值（存 60 = 60 °/s，不是 ×100） |
| `orient_axis_g` | 60 | 朝向判定时重力在某轴上的占比（存 60 = 0.60） |
| `shake_count` | 3 | 摇晃判定窗口内的越阈次数 |
| `shake_window_ms` | 1000 | 摇晃统计窗口（ms） |
| `pickup_still_ms` | 2000 | 抬手判定要求的连续静止时长（ms） |
| `pickup_step_g` | 35 | 抬手判定的加速度阶跃（存 35 = 0.35 g） |
| `pickup_hold_ms` | 800 | 阶跃后等待竖持姿态的窗口（ms） |

- 各 key 读取失败或取值越界（合理范围见 `svc_imu.c`）时回落到上表默认值；默认值与原编译期常量等价，灵敏度不变
- 摇晃与抬手的冷却时间（2000 ms / 3000 ms）是固定行为常量，不入 NVS

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

| 服务 / 模块 | 任务名 | 优先级 | 核心 |
|------|--------|--------|------|
| Event Bus | `event_bus_task` | 4 | 0 |
| Time | `ntp_sync_task` | 2 | 0 |
| Audio | `svc_audio` | 6 | 1 |
| Power | `power_task` | 2 | 0 |
| IMU | `imu_task` | 3 | 0 |
| Storage | `sd_monitor_task` | 3 | 0 |
| Script（Framework） | `script_task` | 4 | 0 |

Wi-Fi / IP 事件由 ESP-IDF 的 `esp_event` 默认事件循环任务处理；`svc_net` 的 HTTP 请求
按需创建临时任务 `svc.http`（优先级 5）。

## 14. 跨服务依赖

- `svc_audio` → `svc_storage`（路径；文件内容经 VFS 直接读）
- `svc_net` → `svc_time`（SNTP 注册）
- `svc_io` → `periph_ext`
- `svc_camera` → `periph_camera`
- `svc_power` → 事件总线（订阅 `SVC_EVENT_TOUCH`、`SVC_EVENT_IMU_*`）
