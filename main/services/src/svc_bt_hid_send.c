/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Services - HID 报告发送（键盘 / 鼠标 / 消费类控制）
 */

#include "svc_bt_hid_send.h"
#include "svc_bt_hid_dev.h"
#include "svc_bt_hid_report.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "svc.bt.hid";

// HID keyboard input report length
#define HID_KEYBOARD_IN_RPT_LEN     8

// HID LED output report length
#define HID_LED_OUT_RPT_LEN         1

// HID mouse input report length
#define HID_MOUSE_IN_RPT_LEN        5

// HID consumer control input report length
#define HID_CC_IN_RPT_LEN           2

esp_err_t svc_bt_hid_dev_register_callbacks(svc_bt_hid_evt_cb_t callbacks)
{
    esp_err_t svc_bt_hid_dev_status;

    if(callbacks != NULL) {
   	    svc_bt_hid_dev_env.svc_bt_hid_dev_cb = callbacks;
    } else {
        return ESP_FAIL;
    }

    if((svc_bt_hid_dev_status = svc_bt_hid_dev_register_cb()) != ESP_OK) {
        return svc_bt_hid_dev_status;
    }

    if((svc_bt_hid_dev_status = esp_ble_gatts_app_register(SVC_BT_HID_APP_ID)) != ESP_OK) {
        return svc_bt_hid_dev_status;
    }

    return svc_bt_hid_dev_status;
}

esp_err_t svc_bt_hid_dev_profile_init(void)
{
     if (svc_bt_hid_dev_env.enabled) {
        ESP_LOGE(TAG, "HID device profile already initialized");
        return ESP_FAIL;
    }
    // Reset the hid device target environment
    memset(&svc_bt_hid_dev_env, 0, sizeof(svc_bt_hid_dev_env_t));
    svc_bt_hid_dev_env.enabled = true;
    return ESP_OK;
}

void svc_bt_hid_send_consumer(uint16_t conn_id, uint8_t key_cmd, bool key_pressed)
{
    uint8_t buffer[HID_CC_IN_RPT_LEN] = {0, 0};
    if (key_pressed) {
        ESP_LOGD(TAG, "svc_bt_hid_consumer_build_report");
        svc_bt_hid_consumer_build_report(buffer, key_cmd);
    }
    ESP_LOGD(TAG, "buffer[0] = %x, buffer[1] = %x", buffer[0], buffer[1]);
    svc_bt_hid_dev_send_report(svc_bt_hid_dev_env.gatt_if, conn_id,
                        HID_RPT_ID_CC_IN, HID_REPORT_TYPE_INPUT, HID_CC_IN_RPT_LEN, buffer);
    return;
}

void svc_bt_hid_send_keyboard(uint16_t conn_id, key_mask_t special_key_mask, uint8_t *keyboard_cmd, uint8_t num_key)
{
    if (num_key > HID_KEYBOARD_IN_RPT_LEN - 2) {
        ESP_LOGE(TAG, "%s(), the number key should not be more than %d", __func__, HID_KEYBOARD_IN_RPT_LEN - 2);
        return;
    }

    uint8_t buffer[HID_KEYBOARD_IN_RPT_LEN] = {0};

    buffer[0] = special_key_mask;

    for (int i = 0; i < num_key; i++) {
        buffer[i+2] = keyboard_cmd[i];
    }

    ESP_LOGD(TAG, "the key vaule = %d,%d,%d, %d, %d, %d,%d, %d", buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]);
    svc_bt_hid_dev_send_report(svc_bt_hid_dev_env.gatt_if, conn_id,
                        HID_RPT_ID_KEY_IN, HID_REPORT_TYPE_INPUT, HID_KEYBOARD_IN_RPT_LEN, buffer);
    return;
}

void svc_bt_hid_send_mouse(uint16_t conn_id, uint8_t mouse_button, int8_t mickeys_x, int8_t mickeys_y)
{
    uint8_t buffer[HID_MOUSE_IN_RPT_LEN];

    buffer[0] = mouse_button;   // Buttons
    buffer[1] = mickeys_x;           // X
    buffer[2] = mickeys_y;           // Y
    buffer[3] = 0;           // Wheel
    buffer[4] = 0;           // AC Pan

    svc_bt_hid_dev_send_report(svc_bt_hid_dev_env.gatt_if, conn_id,
                        HID_RPT_ID_MOUSE_IN, HID_REPORT_TYPE_INPUT, HID_MOUSE_IN_RPT_LEN, buffer);
    return;
}
