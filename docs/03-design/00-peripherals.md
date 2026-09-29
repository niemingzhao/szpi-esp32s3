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
| LCD | `periph_lcd.h` | 显示屏 |
| Touch | `periph_touch.h` | 触摸 |
| Audio | `periph_audio.h` | 音频输入 / 输出 |
| IMU | `periph_imu.h` | 姿态传感器 |
| Storage | `periph_storage.h` | TF 卡 + 内置 Flash |
| Camera | `periph_camera.h` | 摄像头 |
| IO Expander | `periph_io_exp.h` | IO 扩展芯片 |
| Button | `periph_button.h` | BOOT 按键 |

## 3. periph_lcd

### 3.1 接口定义

```c
typedef enum {
    PERIPH_LCD_ROT_0 = 0,
    PERIPH_LCD_ROT_90,
    PERIPH_LCD_ROT_180,
    PERIPH_LCD_ROT_270,
} periph_lcd_rotation_t;

/** 初始化 LCD + LVGL display */
esp_err_t periph_lcd_init(void);

/** 获取 LVGL display 对象（给 framework） */
lv_disp_t *periph_lcd_get_disp(void);

/** 屏幕分辨率 */
uint16_t periph_lcd_get_width(void);
uint16_t periph_lcd_get_height(void);

/** 亮度控制 (0-100, 自动持久化) */
esp_err_t periph_lcd_set_brightness(uint8_t percent);
uint8_t periph_lcd_get_brightness(void);

/** 背光开关 */
esp_err_t periph_lcd_set_backlight(bool on);

/** 屏幕旋转 */
esp_err_t periph_lcd_set_rotation(periph_lcd_rotation_t rot);

/** 全屏颜色填充 */
void periph_lcd_fill(uint16_t color);

/** 画图（从 RAM） */
esp_err_t periph_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t *buf);
```

### 3.2 内部实现要点

- LCD 面板、背光 PWM、IO 扩展的**硬件初始化由 Drivers 层 `bsp_init()` 完成**
- `periph_lcd_init()` 取 `drv_st7789` 的 panel / io handle，调用 `lvgl_port_init()` + `lvgl_port_add_disp()` 集成 LVGL
- 亮度持久化到 NVS namespace `sys`，key `brightness`
- 默认配置：brightness=80
- `periph_lcd_get_disp()` 返回 LVGL display，供 Touch / Framework 使用
- 屏幕旋转：运行时切换 0° / 90° / 180° / 270°，触摸坐标同步旋转

### 3.3 关键参数

| 项 | 值 |
|----|---|
| 分辨率 | 320×240 |
| 像素格式 | RGB565 |
| 帧缓冲 | 10 行高 (10*320*2 = 6.4 KB)，位于内置 DMA 内存 |
| SPI 频率 | 80 MHz |
| 默认旋转 | 0°（横屏，320 为水平方向） |
| 背光 PWM | LEDC_CH0, 5 kHz, 10-bit |

## 4. periph_touch

### 4.1 接口定义

```c
typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
    uint8_t touch_id;       // 触摸 ID（单点固定为 0）
} periph_touch_point_t;

typedef enum {
    PERIPH_TOUCH_EVT_PRESS,
    PERIPH_TOUCH_EVT_RELEASE,
    PERIPH_TOUCH_EVT_TAP,         // 短按
    PERIPH_TOUCH_EVT_DOUBLE_TAP,  // 双击
    PERIPH_TOUCH_EVT_LONG_PRESS,  // 长按
    PERIPH_TOUCH_EVT_SWIPE_LEFT,
    PERIPH_TOUCH_EVT_SWIPE_RIGHT,
    PERIPH_TOUCH_EVT_SWIPE_UP,
    PERIPH_TOUCH_EVT_SWIPE_DOWN,
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
- 手势回调在 `touch_scan_task` 上下文执行，**不可阻塞**
- 单击 / 双击 / 长按通过按下-抬起-时间检测（双击窗口 300 ms）
- 滑动手势通过起止坐标差检测（阈值 50 px）

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
    uint8_t bit_width;        // 16, 24, 32
    uint8_t channels;         // 1, 2
} periph_audio_format_t;

esp_err_t periph_audio_init(void);

/** 设置播放 / 录音格式 */
esp_err_t periph_audio_set_format(periph_audio_dir_t dir, const periph_audio_format_t *fmt);

/** 设置音量 0-100 */
esp_err_t periph_audio_set_volume(uint8_t percent);
uint8_t periph_audio_get_volume(void);

/** 静音开关 */
esp_err_t periph_audio_set_mute(bool mute);

/** 写入 PCM 数据（播放方向） */
esp_err_t periph_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/** 读取 PCM 数据（录音方向） */
esp_err_t periph_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);
```

### 5.2 内部实现要点

- 调用 `drv_es8311_init()` + `drv_es7210_init()`
- 调用 `drv_pca9557_set_pin()` 控制 PA_EN
- 调用音频 codec 抽象库创建设备
- I2S1 配置：MCLK = 384 × SR（16 kHz）/ 256 × SR（48 kHz），16-bit，Stereo
- 内部创建 I2S TX/RX channel（共享时钟）
- 音量映射：0-100 → ES8311 寄存器 0-100（线性）
- 默认：48 kHz / 16-bit / 2ch（音乐），16 kHz / 16-bit / 2ch（录音）

## 6. periph_imu

### 6.1 接口定义

```c
typedef struct {
    float acc_x, acc_y, acc_z;     // m/s²
    float gyr_x, gyr_y, gyr_z;     // °/s
    float roll, pitch, yaw;        // 角度
} periph_imu_data_t;

typedef enum {
    PERIPH_IMU_MOTION_NONE = 0,
    PERIPH_IMU_MOTION_ANY = 0x20,
    PERIPH_IMU_MOTION_NO = 0x40,
    PERIPH_IMU_MOTION_SIGNIFICANT = 0x80,
} periph_imu_motion_t;

esp_err_t periph_imu_init(void);
esp_err_t periph_imu_deinit(void);

/** 同步读取最新数据 */
esp_err_t periph_imu_read(periph_imu_data_t *out);

/** 获取当前运动状态 */
periph_imu_motion_t periph_imu_get_motion(void);

/** 获取朝向（结合重力检测，用于屏幕旋转） */
typedef enum {
    PERIPH_IMU_ORIENTATION_PORTRAIT,
    PERIPH_IMU_ORIENTATION_LANDSCAPE,
    PERIPH_IMU_ORIENTATION_PORTRAIT_FLIP,
    PERIPH_IMU_ORIENTATION_LANDSCAPE_FLIP,
} periph_imu_orientation_t;

periph_imu_orientation_t periph_imu_get_orientation(void);
```

### 6.2 内部实现要点

- 调用 `drv_qmi8658_init()`
- ACC: ±4g, 250 Hz, ODR
- GYR: ±512 dps, 250 Hz, ODR
- 创建 `imu_task`（优先级 3，核心 0，50 Hz 采样）
- 计算欧拉角（基于加速度，简化算法）
- 运动检测由 QMI8658 硬件中断或寄存器轮询

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

- TF 卡：`esp_vfs_fat_sdmmc_mount()` → `/sdcard`（SDMMC 1-bit）
- 内置 Flash：`esp_vfs_spiffs_register()` → `/internal`（`storage` 分区，首次挂载失败时格式化；在 UI 之后挂载以不阻塞首屏）
- 默认自动挂载 TF 卡（无卡时不报错）
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
    PERIPH_CAMERA_RES_QQVGA = 0,   // 160x120
    PERIPH_CAMERA_RES_QVGA,        // 320x240
    PERIPH_CAMERA_RES_VGA,         // 640x480
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

/** 抓取一帧（同步） */
esp_err_t periph_camera_capture(periph_camera_frame_t *frame);
esp_err_t periph_camera_release_frame(periph_camera_frame_t *frame);
```

### 8.2 内部实现要点

- 调用 `drv_gc0308_init()`
- 调用 `drv_pca9557_set_dvp_pwdn()` 上电摄像头
- 使用摄像头驱动抽象库
- 帧缓冲放在 PSRAM
- 默认 QVGA（320×240），JPEG 质量 12（软件编码）

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
    PERIPH_BTN_EVT_VERY_LONG_PRESS,
} periph_button_evt_t;

typedef void (*periph_button_cb_t)(periph_button_evt_t evt, void *user);

esp_err_t periph_button_init(void);
esp_err_t periph_button_register_callback(periph_button_cb_t cb, void *user);
```

### 10.2 内部实现要点

- 调用 `drv_key_init()` 配置 GPIO0 下降沿中断 + 内部上拉
- 创建 `button_task` (优先级 4, 核心 0)
- 区分单击 / 双击 / 长按通过按下时间 + 抬起后的延时

## 11. Peripherals 初始化顺序

`bsp_init()`（Drivers 层）由 `app_main()` 调用，之后调用 `peripherals_init_all()`：

```c
esp_err_t peripherals_init_all(void) {
    ESP_ERROR_CHECK(periph_io_exp_init());    // IO 扩展（先，PA_EN 依赖）
    periph_audio_init();                      // 音频（失败不阻塞启动）
    ESP_ERROR_CHECK(periph_lcd_init());       // LCD + LVGL display
    ESP_ERROR_CHECK(periph_touch_init());     // 触摸（依赖 LCD display）
    periph_imu_init();                        // IMU（失败不阻塞启动）
    periph_storage_init();                    // 存储
    periph_storage_mount(PERIPH_STORAGE_TF_CARD);       // 无卡仅告警
    ESP_ERROR_CHECK(periph_button_init());    // 按键
    return ESP_OK;
}
```

完整的启动序列（`app_main`）：`nvs_flash_init()` → `bsp_init()` → `peripherals_init_all()` → 创建 UI → `periph_storage_mount(PERIPH_STORAGE_INTERNAL_FLASH)`（首次自动格式化，放在 UI 之后以免阻塞首屏）。

## 12. 错误处理

- Peripherals 函数失败时返回 `esp_err_t`
- 关键路径（如 `periph_lcd_init`）调用者用 `ESP_ERROR_CHECK`
- 非关键路径（如 `periph_camera_init` 在无摄像头时）允许 `ESP_FAIL` 返回，调用者记录日志

## 13. 资源使用约束

- **LVGL 帧缓冲**：内置 DMA 内存（SPI 驱动无法直接 DMA PSRAM，见 `AGENTS.md` 4.2）
- **音频 buffer**：优先 PSRAM
- **触摸 buffer**：内置 SRAM（实时性要求）
- **IMU buffer**：内置 SRAM（实时性要求）
- **互斥锁**：每个模块内的全局状态由内部互斥锁保护