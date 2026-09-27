/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Drivers - 公共头
 * 声明所有芯片级驱动的初始化接口
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- drv_i2c - I2C0 主机总线（新版 i2c_master 驱动） ---------- */

#define DRV_I2C_SDA_GPIO   GPIO_NUM_1
#define DRV_I2C_SCL_GPIO   GPIO_NUM_2
#define DRV_I2C_FREQ_HZ    100000    /* 板上各器件的 SCL 频率（新版驱动按设备设置） */

/**
 * @brief 创建 I2C0 主机总线（由 bsp_init 调用，只建一次）
 */
esp_err_t drv_i2c_bus_init(void);

/**
 * @brief 总线句柄（触摸 panel_io 需要直接使用）
 */
i2c_master_bus_handle_t drv_i2c_bus_handle(void);

/**
 * @brief 在总线上挂一个从设备，得到设备句柄
 *
 * @param dev_addr      7 位从机地址
 * @param scl_speed_hz  该设备的 SCL 频率
 */
esp_err_t drv_i2c_device_add(uint16_t dev_addr, uint32_t scl_speed_hz,
                             i2c_master_dev_handle_t *out_dev);

/**
 * @brief 写寄存器：先发寄存器地址，再发数据
 */
esp_err_t drv_i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                            const uint8_t *data, size_t len);

/**
 * @brief 读寄存器：发寄存器地址后重复起始读 len 字节
 */
esp_err_t drv_i2c_read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                           uint8_t *data, size_t len);

/**
 * @brief 裸写：不发寄存器地址，直接发 len 字节（外扩 I2C 器件多用这种帧）
 */
esp_err_t drv_i2c_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len);

/**
 * @brief 裸读：不发寄存器地址，直接收 len 字节
 */
esp_err_t drv_i2c_receive(i2c_master_dev_handle_t dev, uint8_t *data, size_t len);

/**
 * @brief 摘除从设备（外扩 I2C 这类临时挂载 / 摘除的场景用）
 */
esp_err_t drv_i2c_device_remove(i2c_master_dev_handle_t dev);

/* -------------- drv_ledc - LEDC 背光 PWM（GPIO42，反相） -------------- */

/**
 * @brief 初始化 LCD 背光 LEDC
 *
 * 配置: Channel 0, Timer 0, Low Speed, 5 kHz, 10-bit
 * GPIO42 输出，反相 (output_invert=true 因为背光电路是反相的)
 */
esp_err_t drv_ledc_init(void);

/**
 * @brief 设置背光亮度
 * @param percent 0-100
 */
esp_err_t drv_ledc_set_brightness(uint8_t percent);

/* -------------- drv_pca9557 - IO 扩展芯片（I2C @ 0x19） -------------- */

// 引脚定义
#define DRV_PCA9557_LCD_CS     BIT(0)
#define DRV_PCA9557_PA_EN      BIT(1)
#define DRV_PCA9557_DVP_PWDN   BIT(2)

/**
 * @brief 初始化 PCA9557 IO 扩展芯片
 *
 * 配置: BIT0/BIT1/BIT2 设为输出 (CONFIG = 0xF8)
 * 默认: LCD_CS=1, PA_EN=0, DVP_PWDN=1
 */
esp_err_t drv_pca9557_init(void);

/**
 * @brief 设置 PCA9557 某个输出位的电平
 * @param gpio_bit BIT(0)/BIT(1)/BIT(2)
 * @param level 0=低, 1=高
 */
esp_err_t drv_pca9557_set_pin(uint8_t gpio_bit, uint8_t level);

/* --- drv_st7789 - LCD 面板驱动（SPI3，320×240，CS 由 PCA9557.BIT0 控制） --- */

// 面板参数（与硬件规格一致）
#define DRV_LCD_H_RES   320
#define DRV_LCD_V_RES   240

/**
 * @brief 初始化 ST7789 LCD 面板
 *
 * 依赖: drv_pca9557_init()（LCD_CS 走 PCA9557.BIT0）
 * 内部: spi_bus → panel_io → st7789_device → reset/init
 */
esp_err_t drv_st7789_init(void);

/**
 * @brief 取面板句柄（Peripherals 层建 LVGL 显示用）
 */
esp_err_t drv_st7789_get_panel_handle(esp_lcd_panel_handle_t *out_handle);

/**
 * @brief 取面板 IO 句柄（Peripherals 层建 LVGL 显示用）
 */
esp_err_t drv_st7789_get_io_handle(esp_lcd_panel_io_handle_t *out_io_handle);

/* ----------- drv_ft6336 - 触摸芯片驱动（I2C @ 0x38，轮询） ----------- */

/**
 * @brief 初始化 FT6336 触摸芯片
 *
 * 依赖: drv_i2c_bus_init()
 * 内部: 创建 I2C panel_io → ft5x06 device
 */
esp_err_t drv_ft6336_init(void);

/**
 * @brief 取触摸句柄（Peripherals 层注册 LVGL 输入用）
 */
esp_err_t drv_ft6336_get_touch_handle(esp_lcd_touch_handle_t *out_handle);

/* --------- drv_key - BOOT 按键驱动（GPIO0，轮询 + 软件去抖） --------- */

/**
 * @brief 按键事件类型
 */
typedef enum {
    DRV_KEY_EVT_CLICK,          // 单击
    DRV_KEY_EVT_DOUBLE_CLICK,   // 双击
    DRV_KEY_EVT_LONG_PRESS,     // 长按（按住期间达到 1.5 s 即触发）
} drv_key_evt_t;

/**
 * @brief 按键事件回调
 * @param evt 事件类型
 * @param user_data 用户参数
 */
typedef void (*drv_key_cb_t)(drv_key_evt_t evt, void *user_data);

/**
 * @brief 初始化 BOOT 按键 (GPIO0)
 *
 * 轮询 + 去抖判定单击 / 双击 / 长按（长按在按住期间触发），
 * 内部创建 key_task（优先级 4，核心 0）
 */
esp_err_t drv_key_init(void);

/**
 * @brief 注册按键事件回调
 */
esp_err_t drv_key_register_callback(drv_key_cb_t cb, void *user_data);

/* ---------------- drv_qmi8658 - IMU 驱动（I2C @ 0x6A） ---------------- */

/**
 * @brief 初始化 QMI8658 IMU
 *
 * 配置: ACC ±4g 250 Hz, GYR ±512 dps 250 Hz
 */
esp_err_t drv_qmi8658_init(void);

/**
 * @brief 读取 IMU 数据
 *
 * @param out_acc_x/y/z 输出加速度 m/s²
 * @param out_gyr_x/y/z 输出角速度 °/s
 */
esp_err_t drv_qmi8658_read(float *out_acc_x, float *out_acc_y, float *out_acc_z,
                           float *out_gyr_x, float *out_gyr_y, float *out_gyr_z);

/* ------- drv_es8311 - 音频 DAC（I2C0 @ 0x18，MCLK 由 I2S 提供） ------- */

/**
 * @brief 初始化 ES8311（16-bit，MCLK = sample_rate * 256）
 */
esp_err_t drv_es8311_init(uint32_t sample_rate);

/**
 * @brief 运行期切换采样率（需同时重配 I2S 时钟）
 */
esp_err_t drv_es8311_set_sample_rate(uint32_t sample_rate);

/**
 * @brief 设置输出音量 0-100
 */
esp_err_t drv_es8311_set_volume(uint8_t percent);

/**
 * @brief 静音开关
 */
esp_err_t drv_es8311_set_mute(bool mute);

/* --- drv_es7210 - 麦克风 ADC（I2C0 @ 0x41，与 ES8311 共用 I2S 总线） --- */

/**
 * @brief 初始化 ES7210（16-bit、I2S 格式 + 1xFS TDM、MIC 增益 30 dB、bias 2.87 V）
 *
 * 支持 MCLK = 采样率 × 256 的采样率：16 kHz / 44.1 kHz / 48 kHz
 */
esp_err_t drv_es7210_init(uint32_t sample_rate);

/**
 * @brief 运行期切换采样率（需同时重配 I2S 时钟）
 */
esp_err_t drv_es7210_set_sample_rate(uint32_t sample_rate);

/* --- drv_camera - 摄像头（DVP，GC0308 / GC2145，SCCB 复用 I2C0，PWDN 由 PCA9557.BIT2 控制） --- */

/**
 * @brief 初始化摄像头（RGB565 / QVGA / 2 帧缓冲在 PSRAM，传感器由 esp32-camera 识别）
 */
esp_err_t drv_camera_init(void);

/**
 * @brief 取一帧；用完必须调用 drv_camera_return_frame() 归还，否则帧缓冲会耗尽
 */
esp_err_t drv_camera_get_frame(camera_fb_t **out);

/**
 * @brief 归还一帧
 */
void drv_camera_return_frame(camera_fb_t *frame);

/**
 * @brief 切换分辨率（frame_size，如 FRAMESIZE_QVGA / FRAMESIZE_VGA）
 */
esp_err_t drv_camera_set_framesize(framesize_t size);

/**
 * @brief 切换像素格式（RGB565 预览 / JPEG 拍照用）
 */
esp_err_t drv_camera_set_pixformat(pixformat_t format);

/**
 * @brief 反初始化并给摄像头掉电
 */
esp_err_t drv_camera_deinit(void);

/* -------------------------- BSP - 板级初始化 -------------------------- */

/**
 * @brief 按固定顺序初始化所有底层外设
 *
 * 调用顺序: I2C → LEDC 背光 → PCA9557 → ST7789 → FT6336 → BOOT 键 → QMI8658
 * 面板 / 触摸句柄由各驱动的 get_*_handle() 提供，供 Peripherals 层使用。
 *
 * @return ESP_OK 成功
 */
esp_err_t bsp_init(void);

#ifdef __cplusplus
}
#endif
