# HAL 硬件抽象层详细设计

HAL 层屏蔽硬件差异，向 Services 层提供统一的 C 接口。本文档定义每个 HAL 模块的接口、状态、错误处理。

## 1. 设计原则

- **业务语义**：API 用业务术语命名（如 `set_brightness(0-100)`），不暴露硬件寄存器
- **同步优先**：HAL 函数尽量同步（初始化用 `ESP_ERROR_CHECK`），异步部分推到 Services 层
- **可替换**：HAL 实现可以替换（支持其他板子），只需改 drivers 层
- **资源感知**：每个 HAL 模块自己管理资源（互斥锁、缓存）

## 2. HAL 模块清单

| 模块 | 头文件 | 职责 |
|------|--------|------|
| LCD | `hal_lcd.h` | 显示屏 |
| Touch | `hal_touch.h` | 触摸 |
| Audio | `hal_audio.h` | 音频输入 / 输出 |
| IMU | `hal_imu.h` | 姿态传感器 |
| Storage | `hal_storage.h` | TF 卡 + 内置 Flash |
| Camera | `hal_camera.h` | 摄像头 |
| IO Expander | `hal_io_exp.h` | IO 扩展芯片 |
| Button | `hal_button.h` | BOOT 按键 |

## 3. hal_lcd

### 3.1 接口定义

```c
typedef enum {
    HAL_LCD_ROT_0 = 0,
    HAL_LCD_ROT_90,
    HAL_LCD_ROT_180,
    HAL_LCD_ROT_270,
} hal_lcd_rotation_t;

/** 初始化 LCD + LVGL display */
esp_err_t hal_lcd_init(void);

/** 获取 LVGL display 对象（给 framework） */
lv_disp_t *hal_lcd_get_disp(void);

/** 屏幕分辨率 */
uint16_t hal_lcd_get_width(void);
uint16_t hal_lcd_get_height(void);

/** 亮度控制 (0-100, 自动持久化) */
esp_err_t hal_lcd_set_brightness(uint8_t percent);
uint8_t hal_lcd_get_brightness(void);

/** 背光开关 */
esp_err_t hal_lcd_set_backlight(bool on);

/** 屏幕旋转 */
esp_err_t hal_lcd_set_rotation(hal_lcd_rotation_t rot);

/** 全屏颜色填充 */
void hal_lcd_fill(uint16_t color);

/** 画图（从 RAM） */
esp_err_t hal_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const uint16_t *buf);
```

### 3.2 内部实现要点

- 调用 `drv_pca9557_init()` 初始化 IO 扩展
- 调用 `drv_ledc_init()` 初始化背光 PWM
- 调用 `drv_st7789_init()` 初始化 LCD 面板
- 调用 `lvgl_port_init()` + `lvgl_port_add_disp()` 集成 LVGL
- 亮度持久化到 NVS namespace `sys`，key `brightness`
- 默认配置：brightness=80, rotation=HAL_LCD_ROT_0

### 3.3 关键参数

| 项 | 值 |
|----|---|
| 分辨率 | 320×240 |
| 像素格式 | RGB565 |
| 帧缓冲 | 20 行高 (20*320*2 = 12.8 KB) 位于 PSRAM |
| SPI 频率 | 80 MHz |
| 默认旋转 | 0°（与 14-handheld 一致，使 320 是水平方向） |
| 背光 PWM | LEDC_CH0, 5kHz, 10-bit |

## 4. hal_touch

### 4.1 接口定义

```c
typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
    uint8_t touch_id;       // 多点触摸 ID
} hal_touch_point_t;

typedef enum {
    HAL_TOUCH_EVT_PRESS,
    HAL_TOUCH_EVT_RELEASE,
    HAL_TOUCH_EVT_TAP,         // 短按
    HAL_TOUCH_EVT_DOUBLE_TAP,  // 双击
    HAL_TOUCH_EVT_LONG_PRESS,  // 长按
    HAL_TOUCH_EVT_SWIPE_LEFT,
    HAL_TOUCH_EVT_SWIPE_RIGHT,
    HAL_TOUCH_EVT_SWIPE_UP,
    HAL_TOUCH_EVT_SWIPE_DOWN,
} hal_touch_evt_t;

typedef void (*hal_touch_cb_t)(hal_touch_evt_t evt, const hal_touch_point_t *pt, void *user);

esp_err_t hal_touch_init(void);

/** 同步读取最新触摸点 */
esp_err_t hal_touch_read(hal_touch_point_t *point);

/** 注册事件回调（在 LVGL 任务中回调） */
esp_err_t hal_touch_register_callback(hal_touch_cb_t cb, void *user);
```

### 4.2 内部实现要点

- 调用 `drv_ft6336_init()` 初始化触摸 IC
- 创建 `touch_scan_task` (优先级 4, 核心 0) 周期性读取触摸点
- 在任务中检测到手势后调用回调
- 单击 / 双击 / 长按通过按下-抬起-时间检测
- 滑动手势通过起止坐标差检测（阈值 50px）

## 5. hal_audio

### 5.1 接口定义

```c
typedef enum {
    HAL_AUDIO_DIR_PLAY,     // 播放
    HAL_AUDIO_DIR_RECORD,   // 录音
} hal_audio_dir_t;

typedef enum {
    HAL_AUDIO_SR_8K,
    HAL_AUDIO_SR_16K,
    HAL_AUDIO_SR_22K,
    HAL_AUDIO_SR_32K,
    HAL_AUDIO_SR_44K,
    HAL_AUDIO_SR_48K,
} hal_audio_sample_rate_t;

typedef struct {
    hal_audio_sample_rate_t sample_rate;
    uint8_t bit_width;        // 16, 24, 32
    uint8_t channels;         // 1, 2
} hal_audio_format_t;

esp_err_t hal_audio_init(void);

/** 设置播放 / 录音格式 */
esp_err_t hal_audio_set_format(hal_audio_dir_t dir, const hal_audio_format_t *fmt);

/** 设置音量 0-100 */
esp_err_t hal_audio_set_volume(uint8_t percent);
uint8_t hal_audio_get_volume(void);

/** 静音开关 */
esp_err_t hal_audio_set_mute(bool mute);

/** 写入 PCM 数据（播放方向） */
esp_err_t hal_audio_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/** 读取 PCM 数据（录音方向） */
esp_err_t hal_audio_read(uint8_t *data, size_t len, size_t *bytes_read, uint32_t timeout_ms);
```

### 5.2 内部实现要点

- 调用 `drv_es8311_init()` + `drv_es7210_init()`
- 调用 `drv_pca9557_set_pa_en()` 控制功放
- 调用音频 codec 抽象库创建设备
- I2S1 配置：MCLK=384x SR, BCLK=略, 32-bit slot, Stereo
- 内部创建 I2S TX/RX channel（共享时钟）
- 音量映射：0-100 → ES8311 寄存器 0-100（线性）
- 默认：44.1kHz, 16-bit, 2ch（音乐），16kHz, 16-bit, 2ch（录音）

## 6. hal_imu

### 6.1 接口定义

```c
typedef struct {
    float acc_x, acc_y, acc_z;     // m/s²
    float gyr_x, gyr_y, gyr_z;     // °/s
    float roll, pitch, yaw;        // 角度
} hal_imu_data_t;

typedef enum {
    HAL_IMU_MOTION_NONE = 0,
    HAL_IMU_MOTION_ANY = 0x20,
    HAL_IMU_MOTION_NO = 0x40,
    HAL_IMU_MOTION_SIGNIFICANT = 0x80,
} hal_imu_motion_t;

esp_err_t hal_imu_init(void);
esp_err_t hal_imu_deinit(void);

/** 同步读取最新数据 */
esp_err_t hal_imu_read(hal_imu_data_t *out);

/** 获取当前运动状态 */
hal_imu_motion_t hal_imu_get_motion(void);

/** 获取朝向（结合重力检测，用于屏幕旋转） */
typedef enum {
    HAL_IMU_ORIENTATION_PORTRAIT,
    HAL_IMU_ORIENTATION_LANDSCAPE,
    HAL_IMU_ORIENTATION_PORTRAIT_FLIP,
    HAL_IMU_ORIENTATION_LANDSCAPE_FLIP,
} hal_imu_orientation_t;

hal_imu_orientation_t hal_imu_get_orientation(void);
```

### 6.2 内部实现要点

- 调用 `drv_qmi8658_init()`
- ACC: ±4g, 250Hz, ODR
- GYR: ±512 dps, 250Hz, ODR
- 创建 `imu_task` (优先级 3, 核心 0, 50Hz 采样)
- 计算欧拉角（基于加速度，简化算法）
- 运动检测由 QMI8658 硬件中断或寄存器轮询

## 7. hal_storage

### 7.1 接口定义

```c
typedef enum {
    HAL_STORAGE_TF_CARD,         // /sdcard
    HAL_STORAGE_INTERNAL_FLASH,  // /internal (LittleFS)
} hal_storage_type_t;

esp_err_t hal_storage_init(void);

/** 挂载 / 卸载 */
esp_err_t hal_storage_mount(hal_storage_type_t type);
esp_err_t hal_storage_unmount(hal_storage_type_t type);
bool hal_storage_is_mounted(hal_storage_type_t type);

/** 获取挂载状态 */
esp_err_t hal_storage_get_info(hal_storage_type_t type, uint64_t *total_bytes, uint64_t *free_bytes);

/** 通用文件操作（封装 POSIX API） */
esp_err_t hal_storage_file_exists(hal_storage_type_t type, const char *path);
esp_err_t hal_storage_file_size(hal_storage_type_t type, const char *path, size_t *size);
esp_err_t hal_storage_file_delete(hal_storage_type_t type, const char *path);
esp_err_t hal_storage_file_rename(hal_storage_type_t type, const char *from, const char *to);

/** 格式化（谨慎！会清空数据） */
esp_err_t hal_storage_format(hal_storage_type_t type);
```

### 7.2 内部实现要点

- TF 卡：`esp_vfs_fat_sdmmc_mount()` → `/sdcard`
- 内置 Flash：先用 `esp_partition_find_first()` 找到名为 `storage` 的分区，再 `esp_littlefs_mount()`
- 默认自动挂载 TF 卡（无卡时不报错）
- 监控 SD 卡热插拔（可选，Phase 2）

## 8. hal_camera

### 8.1 接口定义

```c
typedef enum {
    HAL_CAMERA_FMT_RGB565,
    HAL_CAMERA_FMT_JPEG,
} hal_camera_fmt_t;

typedef enum {
    HAL_CAMERA_RES_QQVGA = 0,   // 160x120
    HAL_CAMERA_RES_QVGA,        // 320x240
    HAL_CAMERA_RES_VGA,         // 640x480
} hal_camera_res_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    hal_camera_fmt_t fmt;
    uint8_t *buf;
    size_t buf_len;
} hal_camera_frame_t;

esp_err_t hal_camera_init(hal_camera_fmt_t fmt, hal_camera_res_t res);
esp_err_t hal_camera_deinit(void);

/** 抓取一帧（同步） */
esp_err_t hal_camera_capture(hal_camera_frame_t *frame);
esp_err_t hal_camera_release_frame(hal_camera_frame_t *frame);
```

### 8.2 内部实现要点

- 调用 `drv_gc0308_init()`
- 调用 `drv_pca9557_set_dvp_pwdn()` 上电摄像头
- 使用摄像头驱动抽象库
- 帧缓冲放在 PSRAM
- 默认 QVGA + JPEG quality=12

## 9. hal_io_exp

### 9.1 接口定义

```c
typedef enum {
    HAL_IO_LCD_CS,
    HAL_IO_PA_EN,
    HAL_IO_DVP_PWDN,
} hal_io_pin_t;

typedef enum {
    HAL_IO_LEVEL_LOW = 0,
    HAL_IO_LEVEL_HIGH = 1,
} hal_io_level_t;

esp_err_t hal_io_exp_init(void);
esp_err_t hal_io_exp_set(hal_io_pin_t pin, hal_io_level_t level);
hal_io_level_t hal_io_exp_get(hal_io_pin_t pin);
```

### 9.2 内部实现要点

- 调用 `drv_pca9557_init()`
- 默认初始化：LCD_CS=HIGH, PA_EN=LOW, DVP_PWDN=HIGH

## 10. hal_button

### 10.1 接口定义

```c
typedef enum {
    HAL_BTN_EVT_CLICK,
    HAL_BTN_EVT_DOUBLE_CLICK,
    HAL_BTN_EVT_LONG_PRESS,
    HAL_BTN_EVT_VERY_LONG_PRESS,
} hal_button_evt_t;

typedef void (*hal_button_cb_t)(hal_button_evt_t evt, void *user);

esp_err_t hal_button_init(void);
esp_err_t hal_button_register_callback(hal_button_cb_t cb, void *user);
```

### 10.2 内部实现要点

- 调用 `drv_key_init()` 配置 GPIO0 下降沿中断 + 内部上拉
- 创建 `button_task` (优先级 4, 核心 0)
- 区分单击 / 双击 / 长按通过按下时间 + 抬起后的延时

## 11. HAL 初始化顺序

```c
esp_err_t hal_init_all(void) {
    ESP_ERROR_CHECK(bsp_init());  // Drivers 初始化底层（I2C, SPI, LEDC）

    ESP_ERROR_CHECK(hal_io_exp_init());    // IO 扩展（先，所有 codec/PCA 依赖）
    ESP_ERROR_CHECK(hal_audio_init());     // 音频（需要 PA_EN）
    ESP_ERROR_CHECK(hal_lcd_init());       // LCD（需要背光 PWM）
    ESP_ERROR_CHECK(hal_touch_init());     // 触摸（依赖 LCD 尺寸）
    ESP_ERROR_CHECK(hal_imu_init());       // IMU
    ESP_ERROR_CHECK(hal_storage_init());   // 存储
    ESP_ERROR_CHECK(hal_camera_init(...)); // 摄像头（可选）
    ESP_ERROR_CHECK(hal_button_init());    // 按键

    return ESP_OK;
}
```

## 12. 错误处理

- HAL 函数失败时返回 `esp_err_t`
- 关键路径（如 `hal_lcd_init`）调用者用 `ESP_ERROR_CHECK`
- 非关键路径（如 `hal_camera_init` 在无摄像头时）允许 `ESP_FAIL` 返回，调用者记录日志

## 13. 资源使用约束

- **LVGL framebuffer**：必须放 PSRAM
- **音频 buffer**：优先 PSRAM
- **触摸 buffer**：内置 SRAM（实时性要求）
- **IMU buffer**：内置 SRAM（实时性要求）
- **互斥锁**：每个 HAL 模块内的全局状态由内部互斥锁保护