/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Ext 外扩接口（GH1.25）
 *
 * 板上两路 GH1.25：第 1 路是 I2C（与板载 I2C0 共用），第 2 路引出 GPIO10 / GPIO11，
 * 可复用为 GPIO / PWM / UART，并可作为 ADC 输入。GPIO10 / GPIO11 同一时刻只能用于一种
 * 复用（UART 与 PWM 互斥）。不做 CAN。
 *
 * 注意：GPIO11 属 ADC2，与 Wi-Fi 争用，Wi-Fi 打开时 ADC 读数不可用。
 */

#pragma once

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
 * @param freq_hz 1 ~ 100000
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

#ifdef __cplusplus
}
#endif
