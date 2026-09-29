/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Services - HID 报告发送接口
 *
 * 移植自 ESP-IDF 官方例程 ble_hid_device_demo 的 esp_hidd_prf_api.h。
 */

#ifndef SVC_BT_HID_SEND_H__
#define SVC_BT_HID_SEND_H__

#include "esp_bt_defs.h"
#include "esp_gatt_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SVC_BT_HID_EVENT_REG_FINISH = 0,
    SVC_BT_HID_BATTERY_EVENT_REG,
    SVC_BT_HID_EVENT_DEINIT_FINISH,
    SVC_BT_HID_EVENT_BLE_CONNECT,
    SVC_BT_HID_EVENT_BLE_DISCONNECT,
    SVC_BT_HID_EVENT_BLE_VENDOR_REPORT_WRITE_EVT,
    SVC_BT_HID_EVENT_BLE_LED_REPORT_WRITE_EVT,
} svc_bt_hid_evt_t;

/// HID config status
typedef enum {
    SVC_BT_HID_STA_CONN_SUCCESS = 0x00,
    SVC_BT_HID_STA_CONN_FAIL    = 0x01,
} svc_bt_hid_conn_state_t;

/// HID init status
typedef enum {
    SVC_BT_HID_INIT_OK = 0,
    SVC_BT_HID_INIT_FAILED = 1,
} svc_bt_hid_init_state_t;

/// HID deinit status
typedef enum {
    SVC_BT_HID_DEINIT_OK = 0,
    SVC_BT_HID_DEINIT_FAILED = 0,
} svc_bt_hid_deinit_state_t;

#define LEFT_CONTROL_KEY_MASK        (1 << 0)
#define LEFT_SHIFT_KEY_MASK          (1 << 1)
#define LEFT_ALT_KEY_MASK            (1 << 2)
#define LEFT_GUI_KEY_MASK            (1 << 3)
#define RIGHT_CONTROL_KEY_MASK       (1 << 4)
#define RIGHT_SHIFT_KEY_MASK         (1 << 5)
#define RIGHT_ALT_KEY_MASK           (1 << 6)
#define RIGHT_GUI_KEY_MASK           (1 << 7)

typedef uint8_t key_mask_t;
/**
 * @brief HIDD callback parameters union
 */
typedef union {
    /**
	 * @brief SVC_BT_HID_EVENT_INIT_FINISH
	 */
    struct svc_bt_hid_dev_init_finish_evt_param {
        svc_bt_hid_init_state_t state;				/*!< Initial status */
        esp_gatt_if_t gatts_if;
    } init_finish;							      /*!< HID callback param of SVC_BT_HID_EVENT_INIT_FINISH */

    /**
	 * @brief SVC_BT_HID_EVENT_DEINIT_FINISH
	 */
    struct svc_bt_hid_dev_deinit_finish_evt_param {
        svc_bt_hid_deinit_state_t state;				/*!< De-initial status */
    } deinit_finish;								/*!< HID callback param of SVC_BT_HID_EVENT_DEINIT_FINISH */

    /**
     * @brief SVC_BT_HID_EVENT_CONNECT
	 */
    struct svc_bt_hid_dev_connect_evt_param {
        uint16_t conn_id;
        esp_bd_addr_t remote_bda;                   /*!< HID Remote bluetooth connection index */
    } connect;									    /*!< HID callback param of SVC_BT_HID_EVENT_CONNECT */

    /**
     * @brief SVC_BT_HID_EVENT_DISCONNECT
	 */
    struct svc_bt_hid_dev_disconnect_evt_param {
        esp_bd_addr_t remote_bda;                   /*!< HID Remote bluetooth device address */
    } disconnect;									/*!< HID callback param of SVC_BT_HID_EVENT_DISCONNECT */

    /**
     * @brief SVC_BT_HID_EVENT_BLE_VENDOR_REPORT_WRITE_EVT
	 */
    struct svc_bt_hid_dev_vendor_write_evt_param {
        uint16_t conn_id;                           /*!< HID connection index */
        uint16_t report_id;                         /*!< HID report index */
        uint16_t length;                            /*!< data length */
        uint8_t  *data;                             /*!< The pointer to the data */
    } vendor_write;									/*!< HID callback param of SVC_BT_HID_EVENT_BLE_VENDOR_REPORT_WRITE_EVT */

    /**
     * @brief SVC_BT_HID_EVENT_BLE_LED_REPORT_WRITE_EVT
     */
    struct svc_bt_hid_dev_led_write_evt_param {
        uint16_t conn_id;
        uint8_t report_id;
        uint8_t length;
        uint8_t *data;
    } led_write;
} svc_bt_hid_evt_param_t;


/**
 * @brief HID device event callback function type
 * @param event : Event type
 * @param param : Point to callback parameter, currently is union type
 */
typedef void (*svc_bt_hid_evt_cb_t) (svc_bt_hid_evt_t event, svc_bt_hid_evt_param_t *param);



/**
 *
 * @brief           This function is called to receive hid device callback event
 *
 * @param[in]    callbacks: callback functions
 *
 * @return         ESP_OK - success, other - failed
 *
 */
esp_err_t svc_bt_hid_dev_register_callbacks(svc_bt_hid_evt_cb_t callbacks);

/**
 *
 * @brief           This function is called to initialize hid device profile
 *
 * @return          ESP_OK - success, other - failed
 *
 */
esp_err_t svc_bt_hid_dev_profile_init(void);

/**
 *
 * @brief           This function is called to de-initialize hid device profile
 *
 * @return          ESP_OK - success, other - failed
 *
 */
esp_err_t svc_bt_hid_dev_profile_deinit(void);

/**
 *
 * @brief           Get hidd profile version
 *
 * @return          Most 8bit significant is Great version, Least 8bit is Sub version
 *
 */
uint16_t svc_bt_hid_dev_get_version(void);

void svc_bt_hid_send_consumer(uint16_t conn_id, uint8_t key_cmd, bool key_pressed);

void svc_bt_hid_send_keyboard(uint16_t conn_id, key_mask_t special_key_mask, uint8_t *keyboard_cmd, uint8_t num_key);

void svc_bt_hid_send_mouse(uint16_t conn_id, uint8_t mouse_button, int8_t mickeys_x, int8_t mickeys_y);

#ifdef __cplusplus
}
#endif

#endif /* SVC_BT_HID_SEND_H__ */
