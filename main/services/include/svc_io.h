/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IO 外扩接口（GPIO / PWM / I2C / UART / ADC / CAN）
 *
 * 脚本与 App 对 GPIO / PWM / I2C / UART / ADC / CAN 的访问统一走这里，转发到 periph_ext。
 * 只有外扩口的 GPIO10 / GPIO11 两个引脚，同一时刻一个引脚只能用于一种复用（UART 与
 * CAN 各要占用两个引脚）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化外扩 IO
 */
esp_err_t svc_io_init(void);

/**
 * @brief 输出电平（GPIO 模式）
 */
esp_err_t svc_io_gpio_write(uint8_t gpio, uint8_t level);

/**
 * @brief 读取电平（GPIO 模式）
 * @return 0 / 1，失败返回 -1
 */
int svc_io_gpio_read(uint8_t gpio);

/**
 * @brief 输出 PWM（LEDC）
 */
esp_err_t svc_io_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent);

/**
 * @brief 停止 PWM 并释放引脚
 */
esp_err_t svc_io_pwm_stop(uint8_t gpio);

/**
 * @brief 读取 ADC（单位 mV）
 */
esp_err_t svc_io_adc_read(uint8_t gpio, int *out_mv);

/**
 * @brief 在板载 I2C0 总线上读写外部器件（临时挂载，用后摘除）
 */
esp_err_t svc_io_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t svc_io_i2c_read(uint8_t addr, uint8_t *data, size_t len);

/**
 * @brief 配置 UART（占用 GPIO10 / GPIO11，不复用 UART0）
 */
esp_err_t svc_io_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);

/**
 * @brief UART 发送 / 接收
 */
esp_err_t svc_io_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t svc_io_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms);

/**
 * @brief 一帧经典 CAN（数据 0~8 字节）
 */
typedef struct {
    uint32_t id;            /* 11 位标准 ID 或 29 位扩展 ID */
    bool extended;          /* true = 扩展帧 */
    uint8_t len;            /* 数据长度 0~8 */
    uint8_t data[8];
} svc_io_can_frame_t;

/**
 * @brief 配置 CAN（TWAI，占用 GPIO10 / GPIO11，需外接收发器）
 * @param bitrate 位速率 bit/s，如 250000 / 500000 / 1000000
 * @param listen_only true 只监听总线（不使用 TX 引脚、不发送、不应答）
 */
esp_err_t svc_io_can_config(uint32_t bitrate, bool listen_only);

/**
 * @brief 停止 CAN 并释放引脚
 */
esp_err_t svc_io_can_stop(void);

/**
 * @brief 发送一帧（阻塞到发送完成或超时；timeout_ms 为 0 表示一直等）
 */
esp_err_t svc_io_can_send(const svc_io_can_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief 接收一帧（阻塞到收到或超时；timeout_ms 为 0 表示一直等）
 */
esp_err_t svc_io_can_receive(svc_io_can_frame_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
