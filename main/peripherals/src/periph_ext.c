/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - Ext 外扩接口实现
 *
 * GPIO / PWM 用 driver/gpio 与 driver/ledc（PWM 占 TIMER_2/3 + CHANNEL_2/3，避开背光的
 * TIMER_0/CHANNEL_0 与摄像头 XCLK 的 TIMER_1/CHANNEL_1）；I2C 复用板载 I2C0 总线，
 * 临时挂载 / 摘除器件；UART 用 UART1，不复用留给下载与日志的 UART0。
 */

#include "periph_common.h"
#include "periph_ext.h"
#include "drv_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "periph.ext";

#define EXT_UART_PORT           UART_NUM_1
#define EXT_UART_RX_BUF_SIZE    512
#define EXT_I2C_SPEED_HZ        DRV_I2C_FREQ_HZ
#define EXT_PWM_DUTY_RES        LEDC_TIMER_10_BIT
#define EXT_PWM_DUTY_MAX        1023
#define EXT_PWM_FREQ_MAX_HZ     100000
#define EXT_ADC_ATTEN           ADC_ATTEN_DB_12
#define EXT_ADC_RAW_MAX         4095
#define EXT_ADC_VREF_MV         3300

typedef enum {
    EXT_MODE_NONE = 0,
    EXT_MODE_GPIO,
    EXT_MODE_PWM,
    EXT_MODE_UART,
} ext_mode_t;

static bool s_initialized = false;
/* 下标 0 = GPIO10（ADC1_CH9），1 = GPIO11（ADC2_CH0） */
static ext_mode_t s_mode[2] = { EXT_MODE_NONE, EXT_MODE_NONE };
static bool s_uart_installed = false;
static adc_oneshot_unit_handle_t s_adc_unit[2] = { NULL, NULL };
static adc_cali_handle_t s_adc_cali[2] = { NULL, NULL };

static int ext_index(uint8_t gpio)
{
    if (gpio == PERIPH_EXT_GPIO_A) return 0;
    if (gpio == PERIPH_EXT_GPIO_B) return 1;
    return -1;
}

/* 某个引脚要进入 mode：空闲或已是该模式则通过，被别的复用占着则拒绝 */
static esp_err_t ext_claim(uint8_t gpio, ext_mode_t mode)
{
    int idx = ext_index(gpio);
    if (idx < 0) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (s_mode[idx] != EXT_MODE_NONE && s_mode[idx] != mode) {
        ESP_LOGE(TAG, "GPIO%u is busy (mode %d)", gpio, (int)s_mode[idx]);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t periph_ext_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mode[0] = EXT_MODE_NONE;
    s_mode[1] = EXT_MODE_NONE;
    s_initialized = true;
    ESP_LOGI(TAG, "initialized (GPIO%d / GPIO%d)", PERIPH_EXT_GPIO_A, PERIPH_EXT_GPIO_B);
    return ESP_OK;
}

// ============================================================================
// GPIO
// ============================================================================

esp_err_t periph_ext_gpio_write(uint8_t gpio, uint8_t level)
{
    ESP_RETURN_ON_ERROR(ext_claim(gpio, EXT_MODE_GPIO), TAG, "gpio claim failed");

    if (s_mode[ext_index(gpio)] != EXT_MODE_GPIO) {
        const gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio),
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config(%u) failed", gpio);
        s_mode[ext_index(gpio)] = EXT_MODE_GPIO;
    }

    return gpio_set_level(gpio, level ? 1 : 0);
}

int periph_ext_gpio_read(uint8_t gpio)
{
    if (ext_claim(gpio, EXT_MODE_GPIO) != ESP_OK) return -1;

    if (s_mode[ext_index(gpio)] != EXT_MODE_GPIO) {
        const gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio),
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&cfg) != ESP_OK) return -1;
        s_mode[ext_index(gpio)] = EXT_MODE_GPIO;
    }

    return gpio_get_level(gpio);
}

// ============================================================================
// PWM
// ============================================================================

static ledc_timer_t ext_pwm_timer(int idx)
{
    return (idx == 0) ? LEDC_TIMER_2 : LEDC_TIMER_3;
}

static ledc_channel_t ext_pwm_channel(int idx)
{
    return (idx == 0) ? LEDC_CHANNEL_2 : LEDC_CHANNEL_3;
}

esp_err_t periph_ext_pwm_set(uint8_t gpio, uint32_t freq_hz, uint8_t duty_percent)
{
    if (freq_hz == 0 || freq_hz > EXT_PWM_FREQ_MAX_HZ) return ESP_ERR_INVALID_ARG;
    if (duty_percent > 100) duty_percent = 100;

    ESP_RETURN_ON_ERROR(ext_claim(gpio, EXT_MODE_PWM), TAG, "pwm claim failed");

    int idx = ext_index(gpio);
    const ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = ext_pwm_timer(idx),
        .duty_resolution = EXT_PWM_DUTY_RES,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc_timer_config(%u Hz) failed",
                        (unsigned)freq_hz);

    const uint32_t duty = (uint32_t)duty_percent * EXT_PWM_DUTY_MAX / 100;
    const ledc_channel_config_t ccfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = ext_pwm_channel(idx),
        .timer_sel = ext_pwm_timer(idx),
        .duty = duty,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc_channel_config(GPIO%u) failed", gpio);

    s_mode[idx] = EXT_MODE_PWM;
    ESP_LOGI(TAG, "GPIO%u PWM %u Hz duty %u%%", gpio, (unsigned)freq_hz, duty_percent);
    return ESP_OK;
}

esp_err_t periph_ext_pwm_stop(uint8_t gpio)
{
    int idx = ext_index(gpio);
    if (idx < 0) return ESP_ERR_INVALID_ARG;
    if (s_mode[idx] != EXT_MODE_PWM) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ledc_stop(LEDC_LOW_SPEED_MODE, ext_pwm_channel(idx), 0);
    gpio_reset_pin(gpio);           /* 松开 LEDC 输出 */
    s_mode[idx] = EXT_MODE_NONE;
    return err;
}

// ============================================================================
// ADC
// ============================================================================

static esp_err_t ext_adc_ensure(uint8_t gpio, int idx)
{
    if (s_adc_unit[idx] != NULL) return ESP_OK;

    const adc_unit_t unit = (idx == 0) ? ADC_UNIT_1 : ADC_UNIT_2;
    const adc_channel_t chan = (idx == 0) ? ADC_CHANNEL_9 : ADC_CHANNEL_0;

    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&ucfg, &s_adc_unit[idx]), TAG,
                        "adc_oneshot_new_unit(%d) failed", (int)unit);

    const adc_oneshot_chan_cfg_t ccfg = {
        .atten = EXT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_unit[idx], chan, &ccfg), TAG,
                        "adc_oneshot_config_channel failed");

    const adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = unit,
        .chan = chan,
        .atten = EXT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_adc_cali[idx]) != ESP_OK) {
        s_adc_cali[idx] = NULL;
        ESP_LOGW(TAG, "GPIO%u ADC calibration unavailable", gpio);
    }
    return ESP_OK;
}

esp_err_t periph_ext_adc_read(uint8_t gpio, int *out_mv)
{
    if (out_mv == NULL) return ESP_ERR_INVALID_ARG;

    int idx = ext_index(gpio);
    if (idx < 0) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    /* ADC 要引脚没有数字复用 */
    if (s_mode[idx] != EXT_MODE_NONE) return ESP_ERR_INVALID_STATE;

    ESP_RETURN_ON_ERROR(ext_adc_ensure(gpio, idx), TAG, "adc init failed");

    const adc_channel_t chan = (idx == 0) ? ADC_CHANNEL_9 : ADC_CHANNEL_0;
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc_unit[idx], chan, &raw);
    if (err != ESP_OK) {
        /* GPIO11 属 ADC2，Wi-Fi 工作时读不到 */
        ESP_LOGE(TAG, "adc_oneshot_read(GPIO%u) failed: %s", gpio, esp_err_to_name(err));
        return err;
    }

    if (s_adc_cali[idx] != NULL) {
        return adc_cali_raw_to_voltage(s_adc_cali[idx], raw, out_mv);
    }

    *out_mv = raw * EXT_ADC_VREF_MV / EXT_ADC_RAW_MAX;   /* 未标定时按满量程线性折算 */
    return ESP_OK;
}

// ============================================================================
// I2C（复用板载 I2C0，临时挂载 / 摘除）
// ============================================================================

esp_err_t periph_ext_i2c_write(uint8_t addr, const uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev = NULL;

    ESP_RETURN_ON_ERROR(drv_i2c_device_add(addr, EXT_I2C_SPEED_HZ, &dev), TAG,
                        "i2c add 0x%02X failed", addr);

    esp_err_t err = drv_i2c_transmit(dev, data, len);
    drv_i2c_device_remove(dev);
    return err;
}

esp_err_t periph_ext_i2c_read(uint8_t addr, uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev = NULL;

    ESP_RETURN_ON_ERROR(drv_i2c_device_add(addr, EXT_I2C_SPEED_HZ, &dev), TAG,
                        "i2c add 0x%02X failed", addr);

    esp_err_t err = drv_i2c_receive(dev, data, len);
    drv_i2c_device_remove(dev);
    return err;
}

// ============================================================================
// UART（UART1，占用 GPIO10 / GPIO11）
// ============================================================================

esp_err_t periph_ext_uart_config(uint32_t baud, uint8_t data_bits, uint8_t parity, uint8_t stop_bits)
{
    if (baud == 0) return ESP_ERR_INVALID_ARG;
    if (data_bits < 5 || data_bits > 8) return ESP_ERR_INVALID_ARG;
    if (parity > 2) return ESP_ERR_INVALID_ARG;
    if (stop_bits != 1 && stop_bits != 2) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* UART 占两个引脚，两个都得空闲 */
    for (int i = 0; i < 2; i++) {
        if (s_mode[i] != EXT_MODE_NONE && s_mode[i] != EXT_MODE_UART) {
            ESP_LOGE(TAG, "GPIO busy (mode %d), UART unavailable", (int)s_mode[i]);
            return ESP_ERR_INVALID_STATE;
        }
    }

    const uart_config_t ucfg = {
        .baud_rate = (int)baud,
        .data_bits = (uart_word_length_t)(UART_DATA_5_BITS + (data_bits - 5)),
        .parity = (parity == 0) ? UART_PARITY_DISABLE
                                : ((parity == 1) ? UART_PARITY_EVEN : UART_PARITY_ODD),
        .stop_bits = (stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(EXT_UART_PORT, &ucfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(EXT_UART_PORT, PERIPH_EXT_UART_TX, PERIPH_EXT_UART_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

    if (!s_uart_installed) {
        ESP_RETURN_ON_ERROR(uart_driver_install(EXT_UART_PORT, EXT_UART_RX_BUF_SIZE, 0, 0, NULL, 0),
                            TAG, "uart_driver_install failed");
        s_uart_installed = true;
    }

    s_mode[0] = EXT_MODE_UART;
    s_mode[1] = EXT_MODE_UART;
    ESP_LOGI(TAG, "UART%d @ %u bps", EXT_UART_PORT, (unsigned)baud);
    return ESP_OK;
}

esp_err_t periph_ext_uart_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_uart_installed || s_mode[0] != EXT_MODE_UART) return ESP_ERR_INVALID_STATE;

    if (uart_write_bytes(EXT_UART_PORT, data, len) < 0) return ESP_FAIL;

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (uart_wait_tx_done(EXT_UART_PORT, ticks) == ESP_OK) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t periph_ext_uart_read(uint8_t *data, size_t len, size_t *read_len, uint32_t timeout_ms)
{
    if (read_len) *read_len = 0;
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_uart_installed || s_mode[0] != EXT_MODE_UART) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    int n = uart_read_bytes(EXT_UART_PORT, data, len, ticks);
    if (n < 0) return ESP_FAIL;

    if (read_len) *read_len = (size_t)n;
    return ESP_OK;
}
