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
| Bluetooth | `svc_bt.h` / `svc_bt_central.h` | BLE 广播、扫描、GATT 从机 + HID 模拟、中心角色（GATT 客户端） |
| MQTT | `svc_mqtt.h` | MQTT 客户端 |
| WebSocket | `svc_ws.h` | WebSocket 客户端 |
| IO | `svc_io.h` | 外扩 GPIO / PWM / I2C / UART / ADC / CAN |
| Camera | `svc_camera.h` | 摄像头（把 esp32-camera 挡在服务内） |
| Power | `svc_power.h` | 背光、熄屏、唤醒、关机 / 重启 |
| IMU | `svc_imu.h` | 姿态采样与运动事件 |
| Watchdog | `svc_watchdog.h` | Task Watchdog |
| System Info | `svc_sysinfo.h` | 系统信息、崩溃记录 |
| Web 管理页 | `svc_web.h` | 局域网 HTTP 服务：系统状态 + 文件管理（连上 Wi-Fi 自动起） |

另外有一个不是服务、只放产品标识的头：`svc_identity.h` —— 系统名（`SZPI_OS_NAME`）、固件版本
（`SZPI_OS_VERSION`）是宏，配网热点名（`svc_identity_ap_ssid()`）与蓝牙广播名
（`svc_identity_bt_name()`）是函数：两者都拼成 `系统名-地址后 4 位`（大写十六进制），Wi-Fi 用
STA 的 MAC、蓝牙用主机地址，一批板子放一起时靠后缀区分。启动日志、「关于本机」、Wi-Fi 配网、
BLE 广播统一取用；改名 / 改版本只动这两个文件（头 + `svc_identity.c`）。放在 Services 层是因为
Services（网络 / 蓝牙）、Framework、Apps 三个方向都要用，而 Services 是它们共同的下一层。

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
    SVC_EVENT_BT_PAIR_PROMPT,

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
    // BOOT 键（负载为 uint8_t 事件序号：0 单击 / 1 双击 / 2 长按）
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

    // 摄像头（负载为 bool：true = 已打开）
    SVC_EVENT_CAMERA_STATE_CHANGED,

    // 脚本
    SVC_EVENT_SCRIPT_STARTED,
    SVC_EVENT_SCRIPT_STOPPED,
    SVC_EVENT_SCRIPT_FAILED,

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

- 命名空间：`sys`、`wifi`、`script`、`imu`（IMU 判定阈值）、App 私有（如 App 天气用 `weather`）
- 设置变化发布对应的 `SVC_EVENT_*_CHANGED` 事件（如亮度、时区、主题）
- 所有读写走 NVS，进程内不缓存
- `svc_settings_factory_reset()` 擦除整个 NVS 分区，调用后应立即重启

## 4. svc_storage（存储服务）

```c
esp_err_t svc_storage_init(void);

esp_err_t svc_storage_mkdir(const char *path);

/* 整块读写（读上限 1 MB，返回的缓冲由调用方 free） */
esp_err_t svc_storage_read(const char *path, void **out_buf, size_t *out_len);
esp_err_t svc_storage_write(const char *path, const void *data, size_t len);
esp_err_t svc_storage_remove(const char *path);
esp_err_t svc_storage_exists(const char *path, size_t *out_size);

/* 文件管理用（目录递归；都不允许动存储根目录） */
esp_err_t svc_storage_remove_tree(const char *path);
esp_err_t svc_storage_rename(const char *from, const char *to);   /* 同存储内瞬间完成 */
esp_err_t svc_storage_copy(const char *from, const char *to);     /* 分块拷贝，支持目录 */

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

/* 时区：POSIX TZ 串 + 城市显示名，一起存 NVS 的 sys/timezone */
typedef struct {
    char tz[32];        // 如 "CST-8"
    char name[24];      // 城市显示名，可为空
} svc_time_tz_t;
esp_err_t svc_time_set_timezone(const svc_time_tz_t *tz);
esp_err_t svc_time_get_timezone(svc_time_tz_t *out);

esp_err_t svc_time_set_manual(int64_t ts);

bool svc_time_is_synced(void);
int64_t svc_time_now(void);                        // epoch 秒

esp_err_t svc_time_format(int64_t ts, const char *fmt, char *buf, size_t len);
// fmt 同 strftime："%H:%M"、"%-m月%-d日" 等

bool svc_time_get_24h(void);                       // NVS：sys/clock_24h
esp_err_t svc_time_set_24h(bool on);
```

- 12 / 24 小时制放在服务里而不是时钟 App 里：状态栏也要跟着变，两边读同一个设置
- 时间持久化：两层 —— **RTC 保留内存**（`RTC_NOINIT_ATTR`，每秒写：软件复位 / panic / 看门狗后接着走，误差 < 1 s；掉电丢）+ **NVS**（`sys/time_epoch`，每分钟与校时 / 手动设时后写：掉电也不丢，拔电重启后误差 ≤ 1 分钟）。开机取两者中较晚且 ≥ 2020-09 的值恢复，并置为"已同步"。本板没有电池 RTC
- `ntp_sync_task`（优先级 2，核心 0）启动 5 s 后同步一次，之后每 6 小时定时同步一次（无网络时内部直接返回 `ESP_ERR_INVALID_STATE`）；连上 Wi-Fi 时 `wifi_connected_cb` 还会立刻触发一次
- 成功后发布 `SVC_EVENT_TIME_SYNCED`；`svc_time_set_manual()` 也发布它（手动校时同样算"时间可信"）
- 每分钟发布 `SVC_EVENT_TIME_CHANGED`
- 用 `esp_netif_sntp` + `esp_sntp`；时钟 App 的「立即同步网络时间」就是再调一次 `svc_time_sync_ntp()`

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

typedef enum {
    SVC_AUDIO_SR_8K  = 8000,
    SVC_AUDIO_SR_16K = 16000,
    SVC_AUDIO_SR_22K = 22050,
    SVC_AUDIO_SR_32K = 32000,
    SVC_AUDIO_SR_44K = 44100,
    SVC_AUDIO_SR_48K = 48000,
} svc_audio_sample_rate_t;

typedef struct {
    svc_audio_sample_rate_t sample_rate;
    uint8_t bit_width;        // 固定 16
    uint8_t channels;         // 1 / 2
} svc_audio_format_t;

typedef struct {
    svc_audio_src_type_t type;
    const char *uri;
    uint16_t tone_freq;
    uint32_t tone_duration_ms;
} svc_audio_source_t;

esp_err_t svc_audio_init(void);

esp_err_t svc_audio_play(const svc_audio_source_t *src);
esp_err_t svc_audio_pause(void);
esp_err_t svc_audio_resume(void);
esp_err_t svc_audio_stop(void);

esp_err_t svc_audio_record_start(const char *file_path, uint32_t max_seconds);
esp_err_t svc_audio_record_stop(void);

/** 外部 PCM 流播放通路（直接写 I2S，与文件 / 链接播放互斥） */
esp_err_t svc_audio_stream_start(const svc_audio_format_t *fmt);
esp_err_t svc_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t svc_audio_stream_stop(void);

esp_err_t svc_audio_set_volume(uint8_t percent);
uint8_t svc_audio_get_volume(void);
esp_err_t svc_audio_set_mute(bool mute);
bool svc_audio_get_mute(void);

svc_audio_state_t svc_audio_get_state(void);

/** 异步播放 tone（经播放任务排队，会打断当前播放） */
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
- 音频任务的栈只有 4 KB，不要再开大：任务栈必须在内部 RAM，而内部 RAM 要留给相机 DVP 的 7680 字节连续 DMA。链接播放的 TLS 握手栈开销大，改由临时任务（6 KB，用完即退）打开连接与响应头，音频任务只做后续的流式读取
- 播放状态由互斥锁保护，事件回调经队列投递到 `svc_audio`
- tone 用正弦波生成器临时占用播放通路，播完恢复
- 音量与静音都持久化到 NVS（`sys/volume`、`sys/muted`），开机在 `svc_audio_init()` 里读回并应用
- 音量落 NVS 做消抖：`svc_audio_set_volume()` 只记待写值，音频任务空闲满 1 s（或下一条命令进来）才写一次，避免拖动滑块时几十次 `nvs_commit` 卡住 LVGL 任务；静音切换很少发生，`svc_audio_set_mute()` 直接写（状态没变不写）
- 静音只静音输出（`periph_audio_set_mute`），不改音量值 —— 取消静音即恢复原音量，也不会因为静音一次就丢掉用户设好的音量
- **短提示音要等功放启动斜坡**：`play_tone()` 解除功放静音后先等 150 ms（`AUDIO_AMP_SETTLE_MS`）再写 PCM，否则 200 ms 级的提示音会被 NS4150B 的启动斜坡整段吃掉（听起来"没响"）。App 侧只要调 `svc_audio_play_tone_async()`，不要自己算这段等待；两声提示音的间隔要大于（150 ms + 音长），不然后一条会覆盖前一条还没播完的请求（命令队列深度 1）
- 开机提示音（`fw_boot_animation`）先看 `svc_audio_get_mute()`：静音就跳过，不能无条件解除静音（会把持久化的静音清掉）

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
esp_err_t svc_http_get_async(const char *url, char *resp_buf, size_t buf_len,
                             svc_http_cb_t cb, void *user);
esp_err_t svc_http_request(const svc_http_request_t *req, char *resp_buf, size_t buf_len,
                           uint32_t timeout_ms);        /* 通用请求（GET / POST + 请求头 + body） */

/* 异步下载到文件：边收边写、自动跟最多 5 跳重定向；进度回调在 svc.http 任务里 */
esp_err_t svc_http_download(const char *url, const char *path, svc_http_progress_cb_t cb,
                            svc_http_name_cb_t name_cb, void *user);
esp_err_t svc_http_download_pause(void);
esp_err_t svc_http_download_resume(void);
esp_err_t svc_http_download_cancel(void);
```

### 7.3 实现要点

- Wi-Fi / IP 事件由 `esp_event` 默认事件循环任务处理，回调中发布 `SVC_EVENT_WIFI_*`
- STA 不是开机就启动：只有「有保存凭据的自动重连」「SmartConfig」「AP 配网」三条路径会 `esp_wifi_start()`；`svc_net_wifi_scan()` / `svc_net_wifi_connect()` 会自己补一次
- `svc_net_prov_stop()` 回到 STA 模式后要按保存的凭据重新发起连接，否则"配网成功却一直连不上"
- 配网收尾只认网页里提交的目标 SSID（`s_prov_target`），否则 STA 自动连回旧网络的 Connected 事件会在 1~2 s 内把热点关掉
- `svc_net_wifi_forget()` 同时清驱动里的 STA 配置（`esp_wifi_set_config(WIFI_IF_STA, &空)`），不然下次 start 会拿旧配置自动连回去
- SmartConfig 的 `SC_EVENT` 回调在 `svc_net_init()` 里只注册一次，`smartconfig_start()` 不再重复注册；默认配网热点名是 `svc_identity_ap_ssid()`（形如 `SZPI-OS-CA20`，见 `svc_identity.h`）
- 保存一份凭据到 NVS，支持自动重连；连接失败重试若干次后停止并发布 `SVC_EVENT_WIFI_CONNECT_FAILED`
- 开启 PMF capable（兼容 WPA3）
- HTTP 客户端用 `esp_http_client`，HTTPS 走 mbedTLS
- `svc_http_get_async()` 每次调用新建一个临时任务（栈 6 KB，只能用内部 RAM），回调在 `svc.http` 任务里执行（界面自己加 LVGL 锁）。**URL 上限 `SVC_HTTP_URL_MAX` = 512 字节**（天气请求实测 367）：超了直接返回 `ESP_ERR_INVALID_SIZE`；响应缓冲由调用方提供，请求在飞的时候不能释放

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
    uint8_t addr_type;      // BLE_ADDR_TYPE_PUBLIC / RANDOM，连接时要原样传给 svc_bt_central_connect()
    int8_t rssi;
    char name[SVC_BT_NAME_MAX];
} svc_bt_scan_result_t;

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

esp_err_t svc_bt_init(void);          // 必须在 Wi-Fi 之前初始化（要连续内部内存）
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

/* svc_bt_hid.h：BLE HID 设备（键盘 / 鼠标 / 消费类控制） */
esp_err_t svc_bt_hid_init(void);
bool svc_bt_hid_is_ready(void);
esp_err_t svc_bt_hid_key(uint8_t usage, uint8_t modifier);
esp_err_t svc_bt_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy);
esp_err_t svc_bt_hid_consumer(uint8_t usage, bool pressed);
esp_err_t svc_bt_hid_key_click(uint8_t usage, uint8_t modifier);
esp_err_t svc_bt_hid_consumer_click(uint8_t usage);
esp_err_t svc_bt_hid_mouse_click(uint8_t buttons);

/* svc_bt_central.h：中心角色（GATT 客户端，连扫描到的外设） */
typedef struct {
    bool is_128;
    uint16_t uuid16;
    uint8_t uuid128[16];
} svc_bt_uuid_t;

typedef struct {                      // 中心角色事件
    svc_bt_central_evt_t evt;         // CONNECTED / DISCONNECTED / DISCOVER_DONE / READ / WRITE_DONE /
                                      // SUBSCRIBED / NOTIFY / AUTH_DONE
    uint16_t handle;                  // 特征值句柄
    uint16_t reason;                  // 断开原因码（仅 DISCONNECTED 有效）
    esp_err_t err;                    // ESP_OK（正常断开 / 操作成功），或 ESP_FAIL / ESP_ERR_TIMEOUT
    const uint8_t *data;              // READ / NOTIFY 时有效，只在回调期间
    size_t len;
} svc_bt_central_evt_data_t;

typedef void (*svc_bt_central_cb_t)(const svc_bt_central_evt_data_t *evt, void *user);

esp_err_t svc_bt_central_connect(const uint8_t bda[6], uint8_t addr_type);
esp_err_t svc_bt_central_disconnect(void);
bool svc_bt_central_is_connected(void);
esp_err_t svc_bt_central_get_status(svc_bt_central_status_t *out);
esp_err_t svc_bt_central_discover(void);
esp_err_t svc_bt_central_get_services(svc_bt_central_service_t *out, size_t max, size_t *count);
esp_err_t svc_bt_central_get_chars(const svc_bt_central_service_t *svc,
                                   svc_bt_central_char_t *out, size_t max, size_t *count);
esp_err_t svc_bt_central_read(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid);
esp_err_t svc_bt_central_write(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                               const void *data, size_t len, bool with_response);
esp_err_t svc_bt_central_subscribe(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                                   bool enable);
esp_err_t svc_bt_central_register_cb(svc_bt_central_cb_t cb, void *user);
esp_err_t svc_bt_central_unregister_cb(svc_bt_central_cb_t cb, void *user);
```

- 仅 BLE 4.2，Bluedroid；控制器活动实例数保持 IDF 默认
- 从机（peripheral）角色：广播、被中心设备连接、GATT 从机读写与通知（自定义服务 + HID）
- 事件发布 `SVC_EVENT_BT_STATE_CHANGED` / `SVC_EVENT_BT_SCAN_DONE`
- 扫描：`esp_ble_gap_start_scanning(0)` 持续扫描，时长由服务自己的 `esp_timer`（`bt_scan_stop_schedule()`）到点显式 `esp_ble_gap_stop_scanning()` —— 控制器自己按 duration 停不会发 `SCAN_STOP_COMPLETE`，应用会收不到 `SCAN_DONE`
- HID 报告要等配对完成后才能发（`svc_bt_hid_is_ready()`）
- 中心角色（`svc_bt_central.c`）：连接扫描到的外设、发现服务、读写特征与订阅通知；句柄每次连接都会变，所以对外用「服务 / 特征 UUID」定位（本地缓存查询是同步的，请求本身是异步的）
- 同一时刻只允许一次连接尝试：未完成时再调 `svc_bt_central_connect()` 返回 `ESP_ERR_INVALID_STATE`，另有 8 s 超时兜底（发 `DISCONNECTED` + `ESP_ERR_TIMEOUT`）。同时挂多个未完成的连接请求会把 Bluedroid 的 GATTC 连接槽打满（`BT_GATT: Max TCB for gatt_if [N] reached.`），之后所有连接都失败
- 中心角色的结果走 `svc_bt_central_register_cb()` 注册的回调（最多 4 个槽，App 与脚本各占一个；注销用 `unregister_cb()`），回调在 Bluedroid 的 BTC 任务里（栈小）：界面要用 `lv_async_call()` 转回 LVGL 任务，不要在回调里直接调 LVGL
- 订阅：`esp_ble_gattc_register_for_notify()` 传的是特征值句柄，成功后再自己找 CCCD 写 1（通知）/ 2（指示）；通知只转发已订阅的那个句柄
- GATTS / GAP / GATTC 回调都由 `svc_bt` 统一注册，再分发给 HID（GATTS）与 svc_bt_central（GATTC）

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
esp_err_t svc_io_can_config(uint32_t bitrate, bool listen_only);
esp_err_t svc_io_can_stop(void);
esp_err_t svc_io_can_send(const svc_io_can_frame_t *frame, uint32_t timeout_ms);
esp_err_t svc_io_can_receive(svc_io_can_frame_t *out, uint32_t timeout_ms);

/* svc_camera：封装 esp32-camera，App / 脚本不直接依赖该组件 */
esp_err_t svc_camera_init(void);                  /* 服务初始化，不打开硬件 */
esp_err_t svc_camera_open(svc_camera_format_t fmt, svc_camera_size_t size);
esp_err_t svc_camera_close(void);
bool svc_camera_is_open(void);
esp_err_t svc_camera_capture(svc_camera_frame_t *out);
void svc_camera_release(void);
esp_err_t svc_camera_write_bmp(const char *path, const uint16_t *rgb565, uint16_t w, uint16_t h);
```

- `svc_io` 与 `svc_camera` 是薄封装，直接转发到 `periph_ext` / `periph_camera`
- `svc_camera_open()` / `svc_camera_close()` 会发布 `SVC_EVENT_CAMERA_STATE_CHANGED`（负载 bool），状态栏据此点亮 / 熄灭摄像头图标
- `svc_ws` 只收发文本帧，不做二进制帧
- MQTT / WebSocket 使用各自托管组件

## 10. svc_power（电源服务）

```c
esp_err_t svc_power_init(void);

esp_err_t svc_power_set_backlight_timeout(uint32_t seconds);  // 0 = 不超时
uint32_t svc_power_get_backlight_timeout(void);

esp_err_t svc_power_set_brightness(uint8_t percent);          // 0-100，发布 SVC_EVENT_BRIGHTNESS_CHANGED；低于最小可见亮度会被抬上去
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
esp_err_t svc_imu_read(svc_imu_data_t *out);
```

- `imu_task`（优先级 3，核心 0，50 ms 周期）采样
- 判定运动 / 朝向变化，发布 `SVC_EVENT_IMU_MOTION` / `SVC_EVENT_IMU_ORIENTATION`；判定摇晃与抬手，发布 `SVC_EVENT_IMU_SHAKE` / `SVC_EVENT_IMU_PICKUP`
- 抬手判定：设备"平放且静止"（重力在 z 轴上的占比 ≥ 0.92、无运动）达 `pickup_still_ms` 后进入待命；之后屏幕立起来（重力在 z 轴上的占比 ≤ `pickup_tilt_pct`，即倾斜约 30° 以上）并保持 `pickup_hold_ms`，判为"拿起设备"。待命之后**不再重新累计"平放静止"**，所以缓慢拿起（中途有停顿、抖动）也能测到
- 阈值可用 `svc_settings` 的 `imu` 命名空间调整，见下表；服务启动时读一次并缓存，运行期改 NVS 需重启服务（或重启设备）才生效
- 熄屏时用于抬手唤醒（摇晃 / 运动不唤醒，避免碰到就亮屏）

阈值 key（比例类按 100 倍整数存）：

| key | 默认 | 语义 |
|-----|------|------|
| `motion_acc_pct` | 15 | 合加速度偏离 1 g 的百分比（存 15 = 15%） |
| `motion_gyro_dps` | 60 | 角速度阈值（存 60 = 60 °/s，不是 ×100） |
| `orient_axis_g` | 60 | 朝向判定时重力在某轴上的占比（存 60 = 0.60） |
| `shake_count` | 3 | 摇晃判定窗口内的越阈次数 |
| `shake_window_ms` | 1000 | 摇晃统计窗口（ms） |
| `pickup_still_ms` | 1000 | 抬手判定要求的"平放且静止"时长（ms） |
| `pickup_tilt_pct` | 87 | "立起来了"阈值（存 87 = 重力在 z 轴上的占比 < 0.87，即倾斜约 30°） |
| `pickup_hold_ms` | 200 | "立起来"需要保持的时长（ms） |

- 各 key 读取失败或取值越界（合理范围见 `svc_imu.c`）时回落到上表默认值；默认值与原编译期常量等价，灵敏度不变
- 摇晃与抬手的冷却时间（2000 ms / 3000 ms）与抬手的"平放"阈值 0.92 是固定行为常量，不入 NVS

## 12. 服务初始化顺序

```c
esp_err_t services_init(void) {
    if (svc_watchdog_init() != ESP_OK)      ESP_LOGW(TAG, "watchdog unavailable");
    ESP_ERROR_CHECK(svc_event_bus_init());      // 事件总线（所有服务都依赖）
    ESP_ERROR_CHECK(svc_settings_init());       // 配置（NVS）
    ESP_ERROR_CHECK(svc_storage_init());        // 存储
    if (svc_bt_init() != ESP_OK)            ESP_LOGW(TAG, "bluetooth unavailable");  // 必须在 Wi-Fi 之前
    ESP_ERROR_CHECK(svc_time_init());           // 时间
    if (svc_audio_init() != ESP_OK)         ESP_LOGW(TAG, "audio unavailable");
    if (svc_net_init() != ESP_OK)           ESP_LOGW(TAG, "network unavailable");
    ESP_ERROR_CHECK(svc_power_init());          // 电源
    if (svc_imu_init() != ESP_OK)           ESP_LOGW(TAG, "imu unavailable");
    ESP_ERROR_CHECK(svc_io_init());             // 外扩 IO
    ESP_ERROR_CHECK(svc_camera_init());         // 摄像头（不立即开）
    ESP_ERROR_CHECK(svc_sysinfo_init());        // 系统信息（含崩溃记录）
    if (svc_web_init() != ESP_OK)           ESP_LOGW(TAG, "web console unavailable");
    return ESP_OK;
}
```

依赖可选硬件的服务（看门狗 / 蓝牙 / 音频 / 网络 / IMU / Web 控制台）失败只打警告、不阻塞启动；事件总线、配置、存储、时间、电源、IO、摄像头、系统信息失败视为资源或内部状态错误，直接中止启动。

`svc_bt_init()` 必须紧跟 Storage 之后、Wi-Fi 之前：控制器要一块连续内部内存，主机的工作队列与任务栈又只能用内部 RAM，晚于 Wi-Fi / LVGL / 音频就会随机初始化失败。它失败只打警告，不阻塞启动。

日志钩子（`svc_sysinfo_log_capture_start()`）**不在** `services_init()` 里装，而是 `app_main()` 的第一句就装 —— 装在 Services 里太晚，`bsp_init()` / `peripherals_init_all()` 与前面几个服务的日志都进不了日志环，「系统日志」App 的内容会从中间某行开始、跟串口对不上。`svc_sysinfo_init()` 里仍保留一次调用（幂等）做兜底；它负责的是读走上次的崩溃记录并写 NVS。日志环只有最近 2 KB、单行最多 256 字节；抄录前按**当前**剩余栈判断（`xTaskGetStackStart(NULL)` 取栈底 + 当前栈指针求差，阈值 `SYSINFO_LOG_MIN_STACK` = 1024），栈确实不够时跳过该行（只在串口可见），每跳过 64 行在串口提示一次是哪个任务。**不能用 `uxTaskGetStackHighWaterMark()` 判断**：它是历史最低水位，任务在初始化阶段深压过一次就永久偏低，会把该任务之后的日志全部丢掉。

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
| Web 管理页 | `svc.web` | 3 | 不限制 |
| Script（Framework） | `script_task` | 4 | 0 |

Wi-Fi / IP 事件由 ESP-IDF 的 `esp_event` 默认事件循环任务处理；`svc_net` 的 HTTP 请求
按需创建临时任务 `svc.http`（优先级 5）。`svc.web` 只负责起停 HTTP 服务（含 80 端口被配网页占着时的退避重试），请求本身在 `esp_http_server` 自己的任务里处理。

## 14. 跨服务依赖

- `svc_audio` → `svc_storage`（路径；文件内容经 VFS 直接读）
- `svc_net` → `svc_time`（SNTP 注册）
- `svc_io` → `periph_ext`
- `svc_camera` → `periph_camera`
- `svc_power` → 事件总线（订阅 `SVC_EVENT_TOUCH`、`SVC_EVENT_IMU_*`）
- `svc_web` → `svc_net`（Wi-Fi 状态与 IP）、`svc_storage`（文件读写）、`svc_bt`（连接信息）、`svc_sysinfo`（系统信息与日志）

## 15. svc_web（局域网 Web 管理页）

### 15.1 职责

设备连上 Wi-Fi 拿到 IP 后，在 80 端口提供一个网页，局域网里的电脑 / 手机浏览器打开
`http://<设备 IP>/` 即可管理设备；断开 Wi-Fi 就停服务。**不做认证**，只在可信局域网里用。

页面分两个页签：

| 页签 | 内容 |
|------|------|
| 系统状态 | 系统信息（芯片 / 固件 / 构建号 / ESP-IDF / MAC / 运行时长 / 复位原因）、内存（内部 SRAM + PSRAM）、Wi-Fi（状态 / SSID / IP / 信号 / Web 服务）、蓝牙（状态 / 广播 / 已连接设备与地址）、存储容量，以及最近日志（自动刷新，可暂停、可过滤关键字）；右上角「刷新」立即重取一次 |
| 文件管理 | 浏览 TF 卡与内置存储；上传（多选 / 拖拽，逐个带进度）、下载、多选批量删除、重命名、新建文件夹、删除目录（含内容）；复制 / 剪切 / 粘贴（重名自动加 `(N)`）；复制 / 剪切 / 删除在设备上以后台任务执行，界面显示进度；工具栏有「刷新」重读当前目录与容量 |

### 15.2 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 页面本体（`assets/web/index.html`，构建期 `EMBED_FILES` 打进固件） |
| GET | `/api/status` | 状态 JSON（系统 / Wi-Fi / 蓝牙 / 存储） |
| GET | `/api/logs` | 最近日志纯文本（取 `svc_sysinfo` 的日志环，2 KB） |
| GET | `/api/files?path=` | 目录列表 + 该存储容量（JSON，目录在前、按名字排序） |
| GET | `/api/job` | 后台任务进度（`busy` / 种类 / 总数 / 已完成 / 失败数 / 当前项） |
| GET | `/api/download?path=[&dl=1]` | 分块下发文件；不带 `dl` 时按扩展名给 Content-Type，浏览器可内联显示文本 / 图片 / 音频 |
| POST | `/api/upload?dir=&name=` | 请求体就是文件内容，边收边写（`svc_storage_write_open/chunk/close`） |
| POST | `/api/mkdir?path=` | 新建文件夹 |
| POST | `/api/rename?path=&name=` | 同目录改名 |
| POST | `/api/delete` | 请求体一行一个路径，提交**后台任务**批量删除（`svc_storage_remove_tree`），立刻返回 |
| POST | `/api/paste?to=&mode=copy\|cut` | 请求体一行一个来源路径，提交**后台任务**复制 / 剪切，立刻返回 |
| ANY | `/s`、`/s/*` | 脚本注册的网页路由，经 `svc_web_set_custom_handler()` 转给 `fw_script`，只在脚本运行期间有效 |

C 侧接口就这几个：`svc_web_init()`（订阅 Wi-Fi 事件起停服务）、`svc_web_is_running()`（界面显示访问地址用），以及给脚本运行时用的 `svc_web_set_custom_handler()` / `svc_web_clear_custom_handler()`（把 `/s/*` 下的请求转给脚本注册的路由）。

### 15.3 实现要点

- **页面编进固件**，不放文件系统：`main/services/CMakeLists.txt` 用 `EMBED_FILES "assets/web/index.html"`，符号名按 IDF 规则只取文件名（`_binary_index_html_start` / `_end`）。
- **起停与后台任务共用一个任务**（`svc.web`，优先级 3）：事件总线回调只投一条命令。因为配网页也用 80 端口，STA 刚连上时配网 httpd 可能还没停，启动失败要每 500 ms 重试（最多 12 次）；退避期间用 `xQueuePeek` 看有没有停止命令，别把队列里的命令吃掉。
- **耗时操作走后台任务**：复制 / 剪切 / 删除可能涉及成百上千个文件，放在 handler 里会把整个服务堵死（`esp_http_server` 只有一个任务）。所以 handler 只收下路径清单立刻返回，实际动作在 `svc.web` 任务里做，界面轮询 `/api/job` 看进度。
- **上传 / 下载都流式**：整块读写有 1 MB 上限，大文件必须走 `svc_storage_write_open/chunk/close` 与 `svc_storage_stream_read`（8 KB 一块）；写卡复用 `svc_storage_write` 同一条路径（非内部 RAM 的来源先过内部 DMA 缓冲）。
- **关掉 `lru_purge_enable`**：大文件上下传期间状态 / 日志轮询会排在后面，开了 LRU 清理会把排队中的连接踢掉，页面上闪一下"连接断开"。页面侧也做了防堆积（上一次请求没回来就跳过这次轮询）。
- **路径必须落在两个存储里**（`/sdcard`、`/internal`）：页面不做认证，别让它碰到别的挂载点；粘贴时还会拦住"把目录贴进它自己内部"的递归。**内置存储（SPIFFS）不支持 `mkdir`**（IDF 的 VFS 直接返回 `ENOTSUP`），`/api/mkdir` 对 `/internal/*` 回明确错误，页面与文件管理 App 都把「新建文件夹」禁掉。
- **配网期间不起 Web 服务**：配网页占着 80 端口，`svc_web` 收到 `WIFI_CONNECTED` 时先看 `svc_net_prov_is_active()`，配网中直接跳过，等配网结束重连那次再起（否则一直 `error in listen (112)`）。
- **handler 跑在 `esp_http_server` 自己的任务里**（栈 6144），禁止碰 LVGL；拼串 / 转义用的大缓冲统一走服务起来时分配的 PSRAM 共享暂存（handler 顺序执行，共用安全），别做成内部 RAM 的 static 数组。

