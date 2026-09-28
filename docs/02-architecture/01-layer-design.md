# 分层架构详细设计

## 1. 调用规则总则

### 1.1 允许的调用方向

```
Apps       ──→ Framework ──→ Services ──→ Peripherals ──→ Drivers ──→ ESP-IDF
   │            │             │           │         │
   └────────────┴─────────────┴───────────┴─────────┘
                          事件总线（双向）
```

### 1.2 禁止的调用关系

| 调用方 | 被调用方 | 状态 |
|--------|----------|------|
| Peripherals | Services | 禁止 |
| Peripherals | Framework | 禁止 |
| Peripherals | Apps | 禁止 |
| Drivers | Peripherals 及以上 | 禁止 |
| Services | Framework | 禁止 |
| Services | Apps | 禁止 |
| Framework | Apps | 禁止 |
| Apps | Peripherals | 禁止 |
| Apps | Drivers | 禁止 |

### 1.3 例外

- `lvgl_port_lock/unlock` 可在 Peripherals / Services 中使用（LVGL display、触摸 input device、传感器更新）
- `esp_timer` / `FreeRTOS` API 可在任何层使用
- 第三方组件的纯函数可在 Services / Apps 中直接调用
- `fw_input` 可直接注册 `periph_button` 回调获取 BOOT 键事件（按键事件尚未接入事件总线）

## 2. 静态库组织

### 2.1 库依赖关系

```
libszpi_apps.a     (apps/, 由 main/ 静态引用)
        │
        ▼ 依赖
libszpi_framework.a
        │
        ▼ 依赖
libszpi_services.a
        │
        ▼ 依赖
libszpi_peripherals.a
        │
        ▼ 依赖
libszpi_drivers.a
        │
        ▼ 依赖
ESP-IDF + 第三方组件
```

### 2.2 顶层 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS drivers peripherals services framework apps)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(szpi-esp32s3)
```

### 2.3 每层 CMakeLists.txt 模板

```cmake
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES driver esp_lcd esp_timer
)
```

每个层的 `REQUIRES` 字段只能声明下层依赖和真正需要的 ESP-IDF 组件。

## 3. 每层接口定义规则

### 3.1 Drivers 层

**命名规则**：`drv_<chip>_<action>` 或 `bsp_<action>`

**示例**：
```c
esp_err_t drv_pca9557_init(void);
esp_err_t drv_pca9557_set_pin(uint8_t gpio_bit, uint8_t level);
esp_err_t drv_st7789_init(void);
esp_err_t drv_ft6336_init(void);
esp_err_t drv_qmi8658_init(void);
esp_err_t drv_key_init(void);
esp_err_t drv_ledc_init(void);

// 板级初始化，返回 LCD / 触摸 handle 供 Peripherals 层使用
esp_err_t bsp_init(esp_lcd_panel_handle_t *panel,
                   esp_lcd_panel_io_handle_t *io,
                   esp_lcd_touch_handle_t *touch);
```

**约束**：
- 不调用任何 Peripherals / Services / Framework
- 函数全部是阻塞的
- 不创建 FreeRTOS 任务（除了 ISR handler 注册）

### 3.2 Peripherals 层

**命名规则**：`periph_<device>_<action>`

**示例**：
```c
typedef enum {
    PERIPH_LCD_ROT_0 = 0,
    PERIPH_LCD_ROT_90,
    PERIPH_LCD_ROT_180,
    PERIPH_LCD_ROT_270,
} periph_lcd_rotation_t;

esp_err_t periph_lcd_init(void);
esp_err_t periph_lcd_set_brightness(uint8_t percent);
esp_err_t periph_lcd_set_rotation(periph_lcd_rotation_t rot);
uint16_t periph_lcd_get_width(void);
uint16_t periph_lcd_get_height(void);
lv_disp_t *periph_lcd_get_disp(void);  // 唯一向 framework 暴露的 LVGL 对象
```

**约束**：
- 不调用任何 Services / Framework / Apps
- 可以创建 FreeRTOS 任务
- LVGL 相关：`periph_lcd_get_disp` 是唯一向 framework 暴露的 LVGL 对象

### 3.3 Services 层

**命名规则**：`svc_<service>_<action>`

**示例**：
```c
typedef enum {
    SVC_AUDIO_EVT_PLAY_FINISHED,
    SVC_AUDIO_EVT_ERROR,
} svc_audio_evt_t;

typedef void (*svc_audio_cb_t)(svc_audio_evt_t evt, void *user_data);

esp_err_t svc_audio_init(void);
esp_err_t svc_audio_play_file(const char *path);
esp_err_t svc_audio_pause(void);
esp_err_t svc_audio_resume(void);
esp_err_t svc_audio_stop(void);
esp_err_t svc_audio_set_volume(uint8_t percent);
esp_err_t svc_audio_play_tone(uint16_t freq_hz, uint32_t ms);
esp_err_t svc_audio_register_callback(svc_audio_cb_t cb, void *user);
```

**约束**：
- 调用 Peripherals 层
- 每个服务一个 FreeRTOS 任务
- 异步 API + 回调（避免阻塞 LVGL 主线程）
- 同步 API 仅限简单 setter（如音量、亮度）

### 3.4 Framework 层

**命名规则**：`fw_<module>_<action>`

**示例**：
```c
typedef enum {
    FW_APP_LIFECYCLE_CREATE = 0,
    FW_APP_LIFECYCLE_START,
    FW_APP_LIFECYCLE_PAUSE,
    FW_APP_LIFECYCLE_RESUME,
    FW_APP_LIFECYCLE_DESTROY,
} fw_app_lifecycle_t;

typedef struct {
    const char *name;
    const lv_img_dsc_t *icon_64;
    void *(*on_create)(void);
    void  (*on_start)(void *ctx);
    void  (*on_pause)(void *ctx);
    void  (*on_resume)(void *ctx);
    void  (*on_destroy)(void *ctx);
} fw_app_desc_t;

esp_err_t fw_app_mgr_init(void);
esp_err_t fw_app_mgr_register(const fw_app_desc_t *desc);
esp_err_t fw_app_mgr_launch(const char *name);
esp_err_t fw_app_mgr_back_to_home(void);
```

**约束**：
- 调用 Services 层
- 强依赖 LVGL
- 不直接调用 Peripherals（必须经 Services；按键输入的例外见 1.3）
- FW 内部状态由 `fw_*` 模块自己管理

### 3.5 Apps 层

**命名规则**：`app_<name>_<action>`（私有函数）

**示例**：
```c
static lv_obj_t *s_root;
static lv_obj_t *s_time_label;
static lv_timer_t *s_refresh_timer;

static void refresh_cb(lv_timer_t *t) {
    // 更新时间
}

static void *on_create(void) {
    lvgl_port_lock(0);
    s_root = lv_obj_create(NULL);
    s_time_label = lv_label_create(s_root);
    s_refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    lvgl_port_unlock();
    return s_root;
}

static void on_destroy(void *ctx) {
    lvgl_port_lock(0);
    lv_timer_del(s_refresh_timer);
    lv_obj_del((lv_obj_t *)ctx);
    lvgl_port_unlock();
}

FW_APP_REGISTER(
    .name = "Clock",
    .icon_64 = &icon_clock_64,
    .on_create = on_create,
    .on_destroy = on_destroy,
);
```

**约束**：
- 调用 Framework + Services
- 不直接调用 Peripherals
- 通过 `FW_APP_REGISTER` 注册

## 4. 跨层通信：事件总线

### 4.1 接口定义

```c
typedef enum {
    SVC_EVENT_WIFI_CONNECTED,
    SVC_EVENT_WIFI_DISCONNECTED,
    SVC_EVENT_SD_MOUNTED,
    SVC_EVENT_SD_UNMOUNTED,
    SVC_EVENT_TIME_CHANGED,
    SVC_EVENT_THEME_CHANGED,
    SVC_EVENT_LANGUAGE_CHANGED,
    SVC_EVENT_NOTIFICATION_POSTED,
    SVC_EVENT_NOTIFICATION_DISMISSED,
    SVC_EVENT_USER_BASE = 0x8000,
} svc_event_id_t;

typedef struct {
    svc_event_id_t id;
    void *data;
    uint32_t data_len;
} svc_event_t;

typedef void (*svc_event_handler_t)(const svc_event_t *evt, void *user);

esp_err_t svc_event_bus_init(void);
esp_err_t svc_event_bus_subscribe(svc_event_id_t id, svc_event_handler_t h, void *user);
esp_err_t svc_event_bus_unsubscribe(svc_event_id_t id, svc_event_handler_t h);
esp_err_t svc_event_bus_publish(svc_event_id_t id, void *data, uint32_t len);
esp_err_t svc_event_bus_publish_from_isr(svc_event_id_t id, void *data, uint32_t len);
```

### 4.2 分发机制

- 内部使用 FreeRTOS Queue + 单一 dispatcher 任务
- 订阅者通过 handler 回调处理
- 异步发布（不阻塞调用者）

## 5. 配置系统

### 5.1 NVS 命名空间

| Namespace | 用途 |
|-----------|------|
| `sys` | 系统设置（亮度、音量、语言、主题） |
| `wifi` | Wi-Fi 配置（SSID、密码、是否自动重连） |
| `app_<name>` | 每个 App 自己的设置 |
| `ota` | OTA 状态 |

### 5.2 配置 API

```c
esp_err_t svc_settings_init(void);
esp_err_t svc_settings_set_i32(const char *ns, const char *key, int32_t val);
esp_err_t svc_settings_get_i32(const char *ns, const char *key, int32_t *val, int32_t def);
esp_err_t svc_settings_set_str(const char *ns, const char *key, const char *val);
esp_err_t svc_settings_get_str(const char *ns, const char *key, char *buf, size_t len, const char *def);
// ... u8, u32, blob 类型
```

设置变化会发布对应的 `SVC_EVENT_*_CHANGED` 事件。

## 6. 主题与样式

### 6.1 颜色定义（默认深色主题）

```c
#define COLOR_BG_PRIMARY       lv_color_hex(0x121212)
#define COLOR_BG_SECONDARY     lv_color_hex(0x1E1E1E)
#define COLOR_BG_CARD          lv_color_hex(0x2A2A2A)
#define COLOR_TEXT_PRIMARY     lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_SECONDARY   lv_color_hex(0xBBBBBB)
#define COLOR_ACCENT           lv_color_hex(0x4F9EFF)
#define COLOR_ACCENT2          lv_color_hex(0x9C27B0)
#define COLOR_SUCCESS          lv_color_hex(0x4CAF50)
#define COLOR_WARNING          lv_color_hex(0xFFC107)
#define COLOR_ERROR            lv_color_hex(0xF44336)
```

### 6.2 字体

- `lv_font_montserrat_20` - 标准文字
- `lv_font_montserrat_24` - 标题
- `lv_font_montserrat_32` - 大标题
- `font_alipuhui20` - 中文 20px

## 7. 错误处理约定

### 7.1 返回值约定

- 所有公开 API 返回 `esp_err_t`
- 成功：`ESP_OK`
- 失败：`ESP_FAIL` / `ESP_ERR_INVALID_ARG` / `ESP_ERR_NO_MEM` / `ESP_ERR_TIMEOUT` / 自定义 `ESP_ERR_SZPI_*`

### 7.2 自定义错误码

```c
#define ESP_ERR_SZPI_BASE          0x10000
#define ESP_ERR_SZPI_NOT_FOUND     (ESP_ERR_SZPI_BASE + 1)
#define ESP_ERR_SZPI_BUSY          (ESP_ERR_SZPI_BASE + 2)
#define ESP_ERR_SZPI_NOT_SUPPORTED (ESP_ERR_SZPI_BASE + 3)
#define ESP_ERR_SZPI_NOT_MOUNTED   (ESP_ERR_SZPI_BASE + 4)
#define ESP_ERR_SZPI_NO_APP        (ESP_ERR_SZPI_BASE + 5)
```

### 7.3 日志

- 每个模块有自己的 TAG：`TAG = "drv.st7789"`, `TAG = "periph.lcd"`, `TAG = "svc.audio"`, `TAG = "fw.window"`, `TAG = "app.clock"`
- 用 `ESP_LOGI / ESP_LOGW / ESP_LOGE / ESP_LOGD`，禁止 `printf`