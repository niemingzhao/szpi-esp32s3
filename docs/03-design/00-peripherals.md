# Peripherals 外设抽象层详细设计

Peripherals 层屏蔽硬件差异，向 Services 层提供统一的 C 接口。本文档定义每个模块的接口、状态、错误处理。

## 1. 设计原则

- **业务语义**：API 用业务术语命名（如 `set_brightness(0-100)`），不暴露硬件寄存器
- **同步优先**：函数尽量同步（初始化用 `ESP_ERROR_CHECK`），异步部分推到 Services 层
- **可替换**：实现可以替换（支持其他板子），只需改 drivers 层
- **资源感知**：每个模块自己管理资源（互斥锁、缓存）

## 2. 模块清单

| 模块 | 头文件 | 职责 |
|------|--------|------|
| LCD | `periph_lcd.h` | 显示屏 + LVGL display |
| Touch | `periph_touch.h` | 触摸 |
| Audio | `periph_audio.h` | 音频播放 / 录音 |
| IMU | `periph_imu.h` | 姿态传感器 |
| Storage | `periph_storage.h` | TF 卡 + 内置 Flash |
| Camera | `periph_camera.h` | 摄像头（GC0308 / GC2145） |
| IO Expander | `periph_io_exp.h` | IO 扩展芯片 |
| Button | `periph_button.h` | BOOT 按键 |
| Ext | `periph_ext.h` | 外扩接口（GPIO / PWM / I2C / UART / ADC） |

## 3. periph_lcd

### 3.1 接口定义

```c
/** 初始化 LCD + LVGL display */
esp_err_t periph_lcd_init(void);

/** 获取 LVGL display 对象（给 framework） */
lv_display_t *periph_lcd_get_disp(void);

/** 屏幕分辨率 */
uint16_t periph_lcd_get_width(void);
uint16_t periph_lcd_get_height(void);

/** 亮度控制 (0-100, 自动持久化) */
esp_err_t periph_lcd_set_brightness(uint8_t percent);
uint8_t periph_lcd_get_brightness(void);

/** 背光开关 */
esp_err_t periph_lcd_set_backlight(bool on);

/** 全屏颜色填充 */
void periph_lcd_fill(uint16_t color);

/** 画图（从 RAM） */
esp_err_t periph_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t *buf);
```

### 3.2 内部实现要点

- LCD 面板、背光 PWM、IO 扩展的**硬件初始化由 Drivers 层 `bsp_init()` 完成**
- `periph_lcd_init()` 取 `drv_st7789` 的 panel / io handle，调用 `lvgl_port_init()` 并注册 LVGL 显示（esp_lvgl_port 2.x，LVGL 9）
- 亮度持久化到 NVS namespace `sys`，key `brightness`，默认 80
- `periph_lcd_get_disp()` 返回 `lv_display_t`，供 Touch / Framework 使用
- RGB565 字节交换在显示 flush 回调里处理（LVGL 9 已移除 `LV_COLOR_16_SWAP`）
- flush 回调带错误处理：刷屏失败时补一次 `lv_display_flush_ready()`，避免单缓冲下 LVGL 死等

### 3.3 关键参数

| 项 | 值 |
|----|---|
| 分辨率 | 320×240 |
| 像素格式 | RGB565 |
| 绘制缓冲 | 10 行高（320×10×2 = 6.4 KB），位于内置 DMA 内存 |
| 缓冲模式 | 单缓冲 |
| SPI 频率 | 80 MHz |
| 背光 PWM | LEDC CH0, 5 kHz, 10-bit |

## 4. periph_touch

### 4.1 接口定义

```c
typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} periph_touch_point_t;

typedef enum {
    PERIPH_TOUCH_EVT_PRESS,       // 按下
    PERIPH_TOUCH_EVT_RELEASE,     // 抬起
    PERIPH_TOUCH_EVT_TAP,         // 点击
    PERIPH_TOUCH_EVT_LONG_PRESS,  // 长按
} periph_touch_evt_t;

typedef void (*periph_touch_cb_t)(periph_touch_evt_t evt, const periph_touch_point_t *pt, void *user);

esp_err_t periph_touch_init(void);

/** 同步读取最新触摸点 */
esp_err_t periph_touch_read(periph_touch_point_t *point);

/** 注册事件回调（在触摸扫描任务中回调） */
esp_err_t periph_touch_register_callback(periph_touch_cb_t cb, void *user);
```

### 4.2 内部实现要点

- 调用 `drv_ft6336_init()` 初始化触摸 IC
- `periph_touch` 是触摸设备的**唯一轮询者**：创建 `touch_scan_task`（优先级 4，核心 0，10 ms 周期）读取坐标并缓存
- 注册一个 LVGL POINTER input device，其 read_cb 只读取缓存，不访问硬件（避免并发读取丢点）
- 触摸回调在 `touch_scan_task` 上下文执行，**不可阻塞**
- 点击 / 长按通过按下-抬起-时间检测（单击需在长按阈值内抬起）

## 5. periph_audio

### 5.1 接口定义

```c
typedef enum {
    PERIPH_AUDIO_DIR_PLAY,     // 播放
    PERIPH_AUDIO_DIR_RECORD,   // 录音
} periph_audio_dir_t;

typedef enum {
    PERIPH_AUDIO_SR_8K,
    PERIPH_AUDIO_SR_16K,
    PERIPH_AUDIO_SR_22K,
    PERIPH_AUDIO_SR_32K,
    PERIPH_AUDIO_SR_44K,
    PERIPH_AUDIO_SR_48K,
} periph_audio_sample_rate_t;

typedef struct {
    periph_audio_sample_rate_t sample_rate;
    uint8_t bit_width;        // 16
    uint8_t channels;         // 1, 2
} periph_audio_format_t;

esp_err_t periph_audio_init(void);

/** 设置播放 / 录音格式 */
esp_err_t periph_audio_set_format(periph_audio_dir_t dir, const periph_audio_format_t *fmt);

/** 设置音量 0-100 */
esp_err_t periph_audio_set_volume(uint8_t percent);
uint8_t periph_audio_get_volume(void);

/** 静音开关（同时控制功放 PA_EN） */
esp_err_t periph_audio_set_mute(bool mute);

/** 写入 PCM 数据（播放方向） */
esp_err_t periph_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/** 读取 PCM 数据（录音方向） */
esp_err_t periph_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);
```

### 5.2 内部实现要点

- 调用 `drv_es8311_init()`（DAC）+ `drv_es7210_init()`（ADC），两者都是 I2C 侧配置，I2S 数据通路在本模块
- 调用 `drv_pca9557_set_pin()` 控制 PA_EN（静音时关闭功放）
- I2S0：MCLK = 采样率 × 256，16-bit 立体声；发送用标准模式，接收用 TDM
- 采样率：播放支持 8 / 16 / 22.05 / 32 / 44.1 / 48 kHz；录音（ES7210）限 16 / 44.1 / 48 kHz
- 音量映射：0-100 → ES8311 寄存器（线性）
- 默认：播放 48 kHz、录音 16 kHz，16-bit 立体声

## 6. periph_imu

### 6.1 接口定义

```c
typedef struct {
    float acc_x, acc_y, acc_z;     // m/s²
    float gyr_x, gyr_y, gyr_z;     // °/s
    float roll, pitch, yaw;        // 由加速度推算的角度
} periph_imu_data_t;

esp_err_t periph_imu_init(void);
esp_err_t periph_imu_deinit(void);

/** 同步读取最新数据 */
esp_err_t periph_imu_read(periph_imu_data_t *out);
```

### 6.2 内部实现要点

- 调用 `drv_qmi8658_init()`
- ACC ±4g / 250 Hz，GYR ±512 dps / 250 Hz
- 采样任务由 `svc_imu` 负责（50 ms 周期）；periph_imu 只提供读取与角度换算
- 姿态变化 / 摇晃 / 抬手的事件判定在 `svc_imu`

## 7. periph_storage

### 7.1 接口定义

```c
typedef enum {
    PERIPH_STORAGE_TF_CARD,         // /sdcard
    PERIPH_STORAGE_INTERNAL_FLASH,  // /internal (SPIFFS)
} periph_storage_type_t;

/** 存储信息 */
typedef struct {
    uint64_t total_bytes;
    uint64_t free_bytes;
} periph_storage_info_t;

esp_err_t periph_storage_init(void);

/** 挂载 / 卸载 */
esp_err_t periph_storage_mount(periph_storage_type_t type);
esp_err_t periph_storage_unmount(periph_storage_type_t type);
bool periph_storage_is_mounted(periph_storage_type_t type);

/** 查询存储信息 */
esp_err_t periph_storage_get_info(periph_storage_type_t type, periph_storage_info_t *out);

/** 通用文件操作（封装 POSIX API） */
esp_err_t periph_storage_file_exists(periph_storage_type_t type, const char *path);
esp_err_t periph_storage_file_size(periph_storage_type_t type, const char *path, size_t *size);
esp_err_t periph_storage_file_delete(periph_storage_type_t type, const char *path);
esp_err_t periph_storage_file_rename(periph_storage_type_t type, const char *from, const char *to);

/** 格式化（谨慎！会清空数据） */
esp_err_t periph_storage_format(periph_storage_type_t type);
```

### 7.2 内部实现要点

- TF 卡：`esp_vfs_fat_sdmmc_mount()` → `/sdcard`（SDMMC 1-bit，无卡时不报错）
- 内置 Flash：`esp_vfs_spiffs_register()` → `/internal`（`storage` 分区，首次挂载失败时格式化；在首屏之后挂载以不阻塞启动）
- 监控 TF 卡热插拔
- 空间查询：`esp_vfs_fat_info()` / `esp_spiffs_info()`

## 8. periph_camera

### 8.1 接口定义

```c
typedef enum {
    PERIPH_CAMERA_FMT_RGB565,
    PERIPH_CAMERA_FMT_JPEG,
} periph_camera_fmt_t;

typedef enum {
    PERIPH_CAMERA_RES_QVGA = 0,    // 320×240
    PERIPH_CAMERA_RES_VGA,         // 640×480
    PERIPH_CAMERA_RES_UXGA,        // 1600×1200（GC2145）
} periph_camera_res_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    periph_camera_fmt_t fmt;
    uint8_t *buf;
    size_t buf_len;
} periph_camera_frame_t;

esp_err_t periph_camera_init(periph_camera_fmt_t fmt, periph_camera_res_t res);
esp_err_t periph_camera_deinit(void);

/** 抓取一帧（同步），用完必须 release */
esp_err_t periph_camera_capture(periph_camera_frame_t *frame);
esp_err_t periph_camera_release_frame(periph_camera_frame_t *frame);
```

### 8.2 内部实现要点

- 调用 `drv_camera_init()`（esp32-camera，按 PID 自动识别 GC0308 / GC2145）
- 上电 / 掉电经 `drv_pca9557` 控制 DVP_PWDN
- 帧缓冲放 PSRAM，2 帧
- 默认 QVGA（320×240）RGB565，用于预览
- 拍照由相机 App 把 RGB565 帧编码为 BMP 存 TF 卡

## 9. periph_io_exp

### 9.1 接口定义

```c
typedef enum {
    PERIPH_IO_LCD_CS,
    PERIPH_IO_PA_EN,
    PERIPH_IO_DVP_PWDN,
} periph_io_pin_t;

typedef enum {
    PERIPH_IO_LEVEL_LOW = 0,
    PERIPH_IO_LEVEL_HIGH = 1,
} periph_io_level_t;

esp_err_t periph_io_exp_init(void);
esp_err_t periph_io_exp_set(periph_io_pin_t pin, periph_io_level_t level);
periph_io_level_t periph_io_exp_get(periph_io_pin_t pin);
```

### 9.2 内部实现要点

- 调用 `drv_pca9557_init()`
- 默认初始化：LCD_CS=HIGH, PA_EN=LOW, DVP_PWDN=HIGH

## 10. periph_button

### 10.1 接口定义

```c
typedef enum {
    PERIPH_BTN_EVT_CLICK,
    PERIPH_BTN_EVT_DOUBLE_CLICK,
    PERIPH_BTN_EVT_LONG_PRESS,
} periph_button_evt_t;

typedef void (*periph_button_cb_t)(periph_button_evt_t evt, void *user);

esp_err_t periph_button_init(void);
esp_err_t periph_button_register_callback(periph_button_cb_t cb, void *user);
```

### 10.2 内部实现要点

- 调用 `drv_key_init()`：GPIO0 **任意边沿**中断 + 内部上拉
- 驱动内部 `key_task`（优先级 4）做去抖与单击 / 双击 / 长按判定
- 回调在 `key_task` 上下文执行

## 11. periph_ext（外扩接口）

### 11.1 接口定义

外扩接口是 GH1.25 上引出的 GPIO10 / GPIO11，可复用为 GPIO / UART / PWM，并共用板上 I2C；ADC 取 GPIO10 / GPIO11。

```c
esp_err_t periph_ext_init(void);

/** GPIO */
esp_err_t periph_ext_gpio_write(uint8_t gpio, uint8_t level);
int periph_ext_gpio_read(uint8_t gpio);

/** PWM（LEDC，独立定时器 / 通道） */
esp_err_t periph_ext_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent);
esp_err_t periph_ext_pwm_stop(uint8_t gpio);

/** ADC（返回值单位 mV） */
esp_err_t periph_ext_adc_read(uint8_t gpio, int *out_mv);

/** I2C（与板上 I2C0 共用总线，直接读写外部器件） */
esp_err_t periph_ext_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t periph_ext_i2c_read(uint8_t addr, uint8_t *data, size_t len);

/** UART（占用 GPIO10 / GPIO11） */
esp_err_t periph_ext_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);
esp_err_t periph_ext_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t periph_ext_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms);
```

### 11.2 内部实现要点

- GPIO / PWM 用 `driver/gpio`、`driver/ledc`（PWM 用与背光不同的定时器 / 通道）
- I2C 直接复用 `drv_i2c_bus_handle()`（临时挂载 / 摘除设备）
- UART 用 `driver/uart` 的独立端口（不复用 UART0，UART0 留给下载与日志）
- GPIO10 / GPIO11 同一时刻只能用于一种复用（UART 与 PWM 互斥）
- CAN 未实现（外扩接口硬件支持，软件不做）

## 12. Peripherals 初始化顺序

`bsp_init()`（Drivers 层）由 `app_main()` 调用，之后调用 `peripherals_init_all()`：

```c
esp_err_t peripherals_init_all(void) {
    ESP_ERROR_CHECK(periph_io_exp_init());    // IO 扩展（先，PA_EN 依赖）
    periph_audio_init();                      // 音频（失败不阻塞启动）
    ESP_ERROR_CHECK(periph_lcd_init());       // LCD + LVGL display
    ESP_ERROR_CHECK(periph_touch_init());     // 触摸（依赖 LCD display）
    periph_imu_init();                        // IMU（失败不阻塞启动）
    periph_storage_init();                    // 存储
    periph_storage_mount(PERIPH_STORAGE_TF_CARD);   // 无卡仅告警
    ESP_ERROR_CHECK(periph_button_init());    // 按键
    periph_ext_init();                        // 外扩接口（失败不阻塞启动）
    return ESP_OK;
}
```

摄像头不在这里初始化（按需开关，由 `svc_camera` 控制）。

完整启动序列（`app_main`）：`nvs_flash_init()` → `bsp_init()` → `peripherals_init_all()` → 创建 UI → `periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH)`（首次自动格式化，放在首屏之后以免阻塞）。

## 13. 错误处理

- Peripherals 函数失败时返回 `esp_err_t`
- 关键路径（如 `periph_lcd_init`、`periph_touch_init`）调用者用 `ESP_ERROR_CHECK`
- 非关键路径（如 `periph_audio_init`、`periph_camera_init` 无摄像头时）允许 `ESP_FAIL` 返回，调用者记录日志

## 14. 资源使用约束

- **LVGL 绘制缓冲**：内置 DMA 内存（SPI 驱动无法直接 DMA PSRAM，见 `AGENTS.md` 4.2）
- **音频 buffer**：优先 PSRAM
- **触摸 buffer**：内置 SRAM（实时性要求）
- **IMU buffer**：内置 SRAM（实时性要求）
- **互斥锁**：每个模块内的全局状态由内部互斥锁保护
