/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 外扩接口（GH1.25）
 *
 * 板上两路 GH1.25：第 1 路是 I2C（与板载 I2C0 共用），第 2 路引出 GPIO10 / GPIO11，
 * 可复用为 GPIO / PWM / UART / CAN，并可作为 ADC 输入。同一时刻一个引脚只能用于一种
 * 复用；UART 与 CAN 都要同时占用两个引脚。
 *
 * 注意：GPIO11 属 ADC2，与 Wi-Fi 争用，Wi-Fi 打开时 ADC 读数不可用；
 *       CAN 需外接收发器（如 TJA1050）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 外扩多功能口的两个 GPIO */
#define PERIPH_EXT_GPIO_A       10
#define PERIPH_EXT_GPIO_B       11
/** UART 复用时的引脚分配 */
#define PERIPH_EXT_UART_TX      PERIPH_EXT_GPIO_A
#define PERIPH_EXT_UART_RX      PERIPH_EXT_GPIO_B
/** CAN 复用时的引脚分配 */
#define PERIPH_EXT_CAN_TX       PERIPH_EXT_GPIO_A
#define PERIPH_EXT_CAN_RX       PERIPH_EXT_GPIO_B

/**
 * @brief 一帧经典 CAN（数据 0~8 字节）
 */
typedef struct {
    uint32_t id;            /**< 11 位标准 ID 或 29 位扩展 ID */
    bool     extended;      /**< true = 扩展帧 */
    uint8_t  len;           /**< 数据长度 0~8 */
    uint8_t  data[8];
} periph_ext_can_frame_t;

/**
 * @brief 初始化外扩接口（只做状态复位，不做引脚配置）
 */
esp_err_t periph_ext_init(void);

/**
 * @brief 输出电平（GPIO 模式）
 * @param gpio PERIPH_EXT_GPIO_A / PERIPH_EXT_GPIO_B
 * @param level 0 / 1
 */
esp_err_t periph_ext_gpio_write(uint8_t gpio, uint8_t level);

/**
 * @brief 读取电平（GPIO 模式）
 * @return 0 / 1，失败返回 -1
 */
int periph_ext_gpio_read(uint8_t gpio);

/**
 * @brief 输出 PWM（LEDC，独立定时器 / 通道）
 * @param freq_hz 1 ~ 78125（10 bit 分辨率的上限，超出会返回错误）
 * @param duty_percent 0-100
 */
esp_err_t periph_ext_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent);

/**
 * @brief 停止 PWM 并释放该引脚
 */
esp_err_t periph_ext_pwm_stop(uint8_t gpio);

/**
 * @brief 读取 ADC（单位 mV，12 dB 衰减）
 */
esp_err_t periph_ext_adc_read(uint8_t gpio, int *out_mv);

/**
 * @brief 在板载 I2C0 总线上读 / 写外部器件（临时挂载，用后摘除）
 */
esp_err_t periph_ext_i2c_write(uint8_t addr, const uint8_t *data, size_t len);
esp_err_t periph_ext_i2c_read(uint8_t addr, uint8_t *data, size_t len);

/**
 * @brief 配置 UART（占用 GPIO10 / GPIO11，首次调用时安装驱动）
 * @param data_bits 5-8；parity 0=无 1=偶 2=奇；stop_bits 1/2
 */
esp_err_t periph_ext_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);

/**
 * @brief UART 发送
 */
esp_err_t periph_ext_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

/**
 * @brief UART 接收
 */
esp_err_t periph_ext_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms);

/**
 * @brief 配置 CAN（TWAI，占用 GPIO10 / GPIO11，需外接收发器）
 * @param bitrate 位速率 bit/s，如 250000 / 500000 / 1000000
 * @param listen_only true 只监听总线（不使用 TX 引脚、不发送、不应答）
 */
esp_err_t periph_ext_can_config(uint32_t bitrate, bool listen_only);

/**
 * @brief 停止 CAN 并释放引脚
 */
esp_err_t periph_ext_can_stop(void);

/**
 * @brief 发送一帧（阻塞到发送完成或超时；timeout_ms 为 0 表示一直等）
 */
esp_err_t periph_ext_can_send(const periph_ext_can_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief 接收一帧（阻塞到收到或超时；timeout_ms 为 0 表示一直等）
 */
esp_err_t periph_ext_can_receive(periph_ext_can_frame_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
