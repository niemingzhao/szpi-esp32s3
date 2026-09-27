/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Peripherals - 外扩接口实现（GH1.25）
 *
 * GPIO / PWM 用 driver/gpio 与 driver/ledc（PWM 占 TIMER_2/3 + CHANNEL_2/3，避开背光的
 * TIMER_0/CHANNEL_0 与摄像头 XCLK 的 TIMER_1/CHANNEL_1）；I2C 复用板载 I2C0 总线，
 * 临时挂载 / 摘除器件；UART 用 UART1，不复用留给下载与日志的 UART0；
 * CAN 用 TWAI 控制器，占 GPIO10(TX) / GPIO11(RX)，需外接收发器。
 */

#include "periph_common.h"
#include "drv_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "periph.ext";

#define EXT_UART_PORT           UART_NUM_1
#define EXT_UART_RX_BUF_SIZE    512
#define EXT_I2C_SPEED_HZ        DRV_I2C_FREQ_HZ
#define EXT_PWM_DUTY_RES        LEDC_TIMER_10_BIT
#define EXT_PWM_DUTY_MAX        1023
#define EXT_PWM_FREQ_MAX_HZ     78125
#define EXT_ADC_ATTEN           ADC_ATTEN_DB_12
#define EXT_ADC_RAW_MAX         4095
#define EXT_ADC_VREF_MV         3300
#define EXT_CAN_QUEUE_LEN       8

typedef enum {
    EXT_MODE_NONE = 0,
    EXT_MODE_GPIO,
    EXT_MODE_PWM,
    EXT_MODE_UART,
    EXT_MODE_CAN,
} ext_mode_t;

static bool s_initialized = false;
/* 下标 0 = GPIO10（ADC1_CH9），1 = GPIO11（ADC2_CH0） */
static ext_mode_t s_mode[2] = { EXT_MODE_NONE, EXT_MODE_NONE };
static bool s_uart_installed = false;
static adc_oneshot_unit_handle_t s_adc_unit[2] = { NULL, NULL };
static adc_cali_handle_t s_adc_cali[2] = { NULL, NULL };
static twai_node_handle_t s_can_node = NULL;
static QueueHandle_t s_can_rx_queue = NULL;
static SemaphoreHandle_t s_can_tx_mutex = NULL;
static uint8_t s_can_tx_buf[8];
static uint8_t s_can_rx_buf[8];
static bool s_can_err_logged = false;

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

/* -------------------------------- GPIO -------------------------------- */

static esp_err_t ext_gpio_ensure(int idx, uint8_t gpio)
{
    if (s_mode[idx] == EXT_MODE_GPIO) return ESP_OK;

    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config(%u) failed", gpio);
    s_mode[idx] = EXT_MODE_GPIO;
    return ESP_OK;
}

esp_err_t periph_ext_gpio_write(uint8_t gpio, uint8_t level)
{
    ESP_RETURN_ON_ERROR(ext_claim(gpio, EXT_MODE_GPIO), TAG, "gpio claim failed");
    ESP_RETURN_ON_ERROR(ext_gpio_ensure(ext_index(gpio), gpio), TAG, "gpio configure failed");

    return gpio_set_level(gpio, level ? 1 : 0);
}

int periph_ext_gpio_read(uint8_t gpio)
{
    if (ext_claim(gpio, EXT_MODE_GPIO) != ESP_OK) return -1;
    if (ext_gpio_ensure(ext_index(gpio), gpio) != ESP_OK) return -1;

    return gpio_get_level(gpio);
}

/* -------------------------------- PWM -------------------------------- */

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

/* -------------------------------- ADC -------------------------------- */

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

/* --------------- I2C（复用板载 I2C0，临时挂载 / 摘除） --------------- */

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

/* ---------------- UART（UART1，占用 GPIO10 / GPIO11） ---------------- */

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

/* ---------- CAN（TWAI，占用 GPIO10 / GPIO11，需外接收发器） ---------- */

/* ISR：收到一帧就取出来投进队列（队列满则丢弃） */
static bool IRAM_ATTR ext_can_rx_cb(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *user)
{
    (void)edata;
    (void)user;

    twai_frame_t rx = {
        .buffer = s_can_rx_buf,
        .buffer_len = sizeof(s_can_rx_buf),
    };
    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) {
        return false;
    }

    periph_ext_can_frame_t f = { 0 };
    f.id = rx.header.id;
    f.extended = (rx.header.ide != 0);
    f.len = (rx.header.dlc > sizeof(f.data)) ? (uint8_t)sizeof(f.data) : (uint8_t)rx.header.dlc;
    for (uint8_t i = 0; i < f.len; i++) {
        f.data[i] = s_can_rx_buf[i];
    }

    BaseType_t woken = pdFALSE;
    if (s_can_rx_queue != NULL) {
        xQueueSendFromISR(s_can_rx_queue, &f, &woken);
    }
    return (woken == pdTRUE);
}

/* ISR：总线错误只记一条，避免刷屏 */
static bool IRAM_ATTR ext_can_err_cb(twai_node_handle_t node, const twai_error_event_data_t *edata, void *user)
{
    (void)node;
    (void)edata;
    (void)user;

    if (!s_can_err_logged) {
        s_can_err_logged = true;
        ESP_EARLY_LOGW(TAG, "CAN bus error");
    }
    return false;
}

esp_err_t periph_ext_can_config(uint32_t bitrate, bool listen_only)
{
    if (bitrate == 0) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_can_node != NULL) return ESP_ERR_INVALID_STATE;   /* 已配置，先 stop */

    /* 监听模式只用 RX 引脚；普通模式两个引脚都要空闲 */
    const uint8_t pins[2] = { PERIPH_EXT_CAN_TX, PERIPH_EXT_CAN_RX };
    const int first = listen_only ? 1 : 0;
    for (int i = first; i < 2; i++) {
        const int idx = ext_index(pins[i]);
        if (s_mode[idx] != EXT_MODE_NONE && s_mode[idx] != EXT_MODE_CAN) {
            ESP_LOGE(TAG, "GPIO%u is busy (mode %d), CAN unavailable", pins[i], (int)s_mode[idx]);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_can_tx_mutex == NULL) {
        s_can_tx_mutex = xSemaphoreCreateMutex();
        if (s_can_tx_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    if (s_can_rx_queue == NULL) {
        s_can_rx_queue = xQueueCreate(EXT_CAN_QUEUE_LEN, sizeof(periph_ext_can_frame_t));
        if (s_can_rx_queue == NULL) return ESP_ERR_NO_MEM;
    } else {
        xQueueReset(s_can_rx_queue);     /* 清掉上一次会话残留的帧，免得本次一上来就读到旧数据 */
    }
    s_can_err_logged = false;

    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = listen_only ? GPIO_NUM_NC : PERIPH_EXT_CAN_TX,
            .rx = PERIPH_EXT_CAN_RX,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .fail_retry_cnt = 3,
        .tx_queue_depth = EXT_CAN_QUEUE_LEN,
    };
    cfg.bit_timing.bitrate = bitrate;
    cfg.flags.enable_listen_only = listen_only ? 1 : 0;

    esp_err_t err = twai_new_node_onchip(&cfg, &s_can_node);
    if (err != ESP_OK) {
        s_can_node = NULL;
        ESP_LOGE(TAG, "twai_new_node_onchip failed: %s", esp_err_to_name(err));
        return err;
    }

    const twai_event_callbacks_t cbs = {
        .on_rx_done = ext_can_rx_cb,
        .on_error = ext_can_err_cb,
    };
    err = twai_node_register_event_callbacks(s_can_node, &cbs, NULL);
    if (err == ESP_OK) {
        err = twai_node_enable(s_can_node);
    }
    if (err != ESP_OK) {
        twai_node_delete(s_can_node);
        s_can_node = NULL;
        ESP_LOGE(TAG, "CAN start failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = first; i < 2; i++) {
        s_mode[ext_index(pins[i])] = EXT_MODE_CAN;
    }
    ESP_LOGI(TAG, "CAN %u bit/s%s", (unsigned)bitrate, listen_only ? " (listen only)" : "");
    return ESP_OK;
}

esp_err_t periph_ext_can_stop(void)
{
    if (s_can_node == NULL) return ESP_OK;

    esp_err_t err = twai_node_disable(s_can_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN disable failed: %s", esp_err_to_name(err));
        return err;
    }
    err = twai_node_delete(s_can_node);
    s_can_node = NULL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN delete failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < 2; i++) {
        if (s_mode[i] == EXT_MODE_CAN) s_mode[i] = EXT_MODE_NONE;
    }
    ESP_LOGI(TAG, "CAN stopped");
    return ESP_OK;
}

esp_err_t periph_ext_can_send(const periph_ext_can_frame_t *frame, uint32_t timeout_ms)
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    if (frame->len > sizeof(s_can_tx_buf)) return ESP_ERR_INVALID_ARG;
    if (s_can_node == NULL || s_can_tx_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_can_tx_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;

    for (uint8_t i = 0; i < frame->len; i++) {
        s_can_tx_buf[i] = frame->data[i];
    }

    twai_frame_t tx = { 0 };
    tx.header.id = frame->id;
    tx.header.dlc = frame->len;
    tx.header.ide = frame->extended ? 1 : 0;
    tx.buffer = s_can_tx_buf;
    tx.buffer_len = frame->len;

    const int wait_ms = (timeout_ms == 0) ? -1 : (int)timeout_ms;
    esp_err_t err = twai_node_transmit(s_can_node, &tx, wait_ms);
    if (err == ESP_OK) {
        /* 驱动持有数据缓冲，等发送结束再放手 */
        err = twai_node_transmit_wait_all_done(s_can_node, wait_ms);
    }

    xSemaphoreGive(s_can_tx_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN send failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t periph_ext_can_receive(periph_ext_can_frame_t *out, uint32_t timeout_ms)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (s_can_rx_queue == NULL) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive(s_can_rx_queue, out, ticks) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}
