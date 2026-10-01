# 分层架构详细设计

## 1. 调用规则总则

### 1.1 允许的调用方向

```
Apps       ──→ Framework ──→ Services ──→ Peripherals ──→ Drivers ──→ ESP-IDF
脚本       ──↗
   │            │             │
   └────────────┴─────────────┴──→ svc_event_bus（订阅 / 发布，双向）
```

### 1.2 禁止的调用关系

| 调用方 | 被调用方 | 状态 |
|--------|----------|------|
| Peripherals | Services | 禁止 |
| Peripherals | Framework | 禁止 |
| Peripherals | Apps | 禁止 |
| Drivers | Peripherals 及以上 | 禁止 |
| Services | Framework | 禁止 |
| Services | Apps / 脚本 | 禁止 |
| Framework | Apps / 脚本 | 禁止 |
| Apps | Peripherals | 禁止 |
| Apps | Drivers | 禁止 |
| 脚本 | Peripherals / Drivers | 禁止 |

### 1.3 例外

- `lvgl_port_lock/unlock` 可在 Peripherals / Services 中使用（LVGL display、触摸 input device）
- `esp_timer` / `FreeRTOS` API 可在任何层使用
- 第三方组件的纯函数可在 Services / Framework / Apps 中直接调用
- `fw_input` 可直接注册 `periph_button` 回调获取 BOOT 键事件（按键事件不经事件总线）
- 脚本对硬件（GPIO / PWM / I2C / UART / ADC）的访问经 `svc_io`，不直接调用 Peripherals

## 2. 静态库组织

### 2.1 库依赖关系

```
libszpi_apps.a     (main/apps/, 由 main 静态引用)
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

set(EXTRA_COMPONENT_DIRS
    main/drivers main/peripherals main/services main/framework main/apps)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(szpi-esp32s3)
```

### 2.3 每层 CMakeLists.txt 模板

```cmake
idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES <下层组件> <需要的 ESP-IDF 组件>
)
```

每层的 `REQUIRES` 只能声明下层依赖和真正需要的 ESP-IDF 组件。

## 3. 每层接口定义规则

### 3.1 Drivers 层

**命名规则**：`drv_<chip>_<action>` 或 `bsp_<action>`

```c
esp_err_t drv_pca9557_init(void);
esp_err_t drv_pca9557_set_pin(uint8_t gpio_bit, uint8_t level);
esp_err_t drv_st7789_init(void);
esp_err_t drv_ft6336_init(void);
esp_err_t drv_qmi8658_init(void);
esp_err_t drv_key_init(void);
esp_err_t drv_ledc_init(void);
esp_err_t drv_camera_init(void);   /* 兼容 GC0308 / GC2145 */

esp_err_t bsp_init(esp_lcd_panel_handle_t *panel,
                   esp_lcd_panel_io_handle_t *io,
                   esp_lcd_touch_handle_t *touch);
```

**约束**：
- 不调用任何 Peripherals / Services / Framework
- 函数全部阻塞
- 不创建 FreeRTOS 任务（ISR 注册除外）

### 3.2 Peripherals 层

**命名规则**：`periph_<device>_<action>`

```c
esp_err_t periph_lcd_init(void);
esp_err_t periph_lcd_set_brightness(uint8_t percent);
lv_display_t *periph_lcd_get_disp(void);   /* 唯一向 framework 暴露的 LVGL 对象 */

esp_err_t periph_ext_gpio_write(uint8_t gpio, uint8_t level);
esp_err_t periph_ext_adc_read(uint8_t gpio, int *out_mv);
```

**约束**：
- 不调用任何 Services / Framework / Apps
- 可以创建 FreeRTOS 任务
- `periph_lcd_get_disp` 是唯一向 framework 暴露的 LVGL 对象

### 3.3 Services 层

**命名规则**：`svc_<service>_<action>`

```c
esp_err_t svc_audio_init(void);
esp_err_t svc_audio_play(const svc_audio_source_t *src);
esp_err_t svc_audio_stop(void);
esp_err_t svc_audio_set_volume(uint8_t percent);
esp_err_t svc_audio_play_tone_async(uint16_t freq_hz, uint32_t ms);

esp_err_t svc_io_gpio_write(uint8_t gpio, uint8_t level);
esp_err_t svc_io_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent);
```

**约束**：
- 调用 Peripherals 层
- 每个服务独占一个 FreeRTOS 任务
- 异步 API + 回调（避免阻塞 LVGL 任务）
- 同步 API 仅限简单 setter（音量、亮度、GPIO）

### 3.4 Framework 层

**命名规则**：`fw_<module>_<action>`

```c
typedef struct {
    const char *name;
    const lv_image_dsc_t *icon_64;
    const char *symbol;
    void *(*on_create)(void);
    void  (*on_start)(void *ctx);
    void  (*on_pause)(void *ctx);
    void  (*on_resume)(void *ctx);
    void  (*on_destroy)(void *ctx);
    bool  (*on_back)(void *ctx);
} fw_app_desc_t;

esp_err_t fw_app_mgr_init(void);
esp_err_t fw_app_mgr_register(const fw_app_desc_t *desc);
esp_err_t fw_app_mgr_launch(const char *name);
esp_err_t fw_app_mgr_back_to_home(void);
```

**约束**：
- 调用 Services 层
- 强依赖 LVGL
- 不直接调用 Peripherals（按键例外见 1.3）
- 模块内部状态由 `fw_*` 自己管理

### 3.5 Apps 层

**命名规则**：`app_<name>_<action>`（私有函数）

```c
static lv_obj_t *s_root;
static lv_timer_t *s_timer;

static void *on_create(void)
{
    lvgl_port_lock(0);
    s_root = fw_ui_page(NULL);
    s_timer = lv_timer_create(refresh_cb, 1000, NULL);
    lvgl_port_unlock();
    return s_root;
}

static void on_destroy(void *ctx)
{
    lvgl_port_lock(0);
    lv_timer_del(s_timer);
    lv_obj_del(s_root);
    lvgl_port_unlock();
}

const fw_app_desc_t app_xxx_desc = {
    .name = "Xxx",
    .symbol = LV_SYMBOL_FILE,
    .on_create = on_create,
    .on_destroy = on_destroy,
};
```

**约束**：
- 调用 Framework + Services
- 不直接调用 Peripherals
- 通过 `fw_app_mgr_register` 注册
- 不使用大块 static 缓冲（内部 RAM 紧张）

### 3.6 脚本（fw_script）

**Lua 侧命名**：模块名小写，如 `luaopen_ui`、`luaopen_file`

```c
esp_err_t fw_script_init(void);
esp_err_t fw_script_list(fw_script_entry_t *out, size_t max, size_t *count);
esp_err_t fw_script_run(const char *path);
esp_err_t fw_script_stop(void);
bool fw_script_is_running(void);
```

**约束**：
- 运行时、脚本管理、绑定统一放在 `fw_script`
- 绑定调用 Framework 自身与 Services（硬件经 `svc_io`）
- 同一时刻只运行一个前台脚本
- 脚本错误不得影响系统

## 4. 跨层通信：事件总线

### 4.1 事件 ID

```c
typedef enum {
    SVC_EVENT_BASE = 0,

    /* 存储 */
    SVC_EVENT_SD_MOUNTED,
    SVC_EVENT_SD_UNMOUNTED,

    /* 网络 */
    SVC_EVENT_WIFI_CONNECTING,
    SVC_EVENT_WIFI_CONNECTED,
    SVC_EVENT_WIFI_DISCONNECTED,
    SVC_EVENT_WIFI_CONNECT_FAILED,

    /* 蓝牙 BLE */
    SVC_EVENT_BT_STATE_CHANGED,
    SVC_EVENT_BT_SCAN_DONE,

    /* 时间 */
    SVC_EVENT_TIME_SYNCED,
    SVC_EVENT_TIME_CHANGED,
    SVC_EVENT_TIMEZONE_CHANGED,

    /* 主题 */
    SVC_EVENT_THEME_CHANGED,

    /* 电源 */
    SVC_EVENT_BRIGHTNESS_CHANGED,
    SVC_EVENT_TOUCH,

    /* IMU */
    SVC_EVENT_IMU_MOTION,
    SVC_EVENT_IMU_ORIENTATION,

    /* 音频 */
    SVC_EVENT_AUDIO_PLAYBACK_STARTED,
    SVC_EVENT_AUDIO_PLAYBACK_FINISHED,
    SVC_EVENT_AUDIO_PLAYBACK_ERROR,
    SVC_EVENT_AUDIO_RECORD_STARTED,
    SVC_EVENT_AUDIO_RECORD_FINISHED,

    /* 脚本 */
    SVC_EVENT_SCRIPT_STARTED,
    SVC_EVENT_SCRIPT_STOPPED,

    SVC_EVENT_USER_BASE = 0x8000,
} svc_event_id_t;
```

### 4.2 接口

```c
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

### 4.3 分发机制

- 内部用 FreeRTOS Queue + 单一 dispatcher 任务
- 订阅者通过 handler 回调处理
- 异步发布，不阻塞调用者；回调里禁止阻塞过久

## 5. 配置系统

### 5.1 NVS 命名空间

| Namespace | 用途 |
|-----------|------|
| `sys` | 系统设置（亮度、音量、主题、熄屏时间） |
| `wifi` | Wi-Fi 配置（SSID、密码、自动重连） |
| `script` | 脚本系统设置 |
| `app_<name>` | 每个 App 自己的设置 |

### 5.2 配置 API

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

设置变化会发布对应的 `SVC_EVENT_*_CHANGED` 事件。

## 6. 主题与字体

### 6.1 调色板

颜色统一经 `fw_theme_color_*()` 取，界面代码不写死颜色：

```c
lv_color_t fw_theme_color_bg_primary(void);
lv_color_t fw_theme_color_bg_card(void);
lv_color_t fw_theme_color_text_primary(void);
lv_color_t fw_theme_color_accent(void);
lv_color_t fw_theme_color_border(void);
```

深色 / 浅色两套取值见 `docs/03-design/02-ui-system.md`。

### 6.2 字体

- 中文：`fw_asset_font_cn()`（14 px 正文）、`fw_asset_font_cn_large()`（16 px 标题）
- 拉丁 / 数字：`fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()`
- 界面文案的汉字必须在字体子集内

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
#define ESP_ERR_SZPI_SCRIPT        (ESP_ERR_SZPI_BASE + 6)
```

### 7.3 日志

- 每个模块有自己的 TAG：`"drv.st7789"`、`"periph.lcd"`、`"svc.audio"`、`"fw.script"`、`"app.clock"`
- 统一用 `ESP_LOGI / ESP_LOGW / ESP_LOGE / ESP_LOGD`，禁止 `printf`
