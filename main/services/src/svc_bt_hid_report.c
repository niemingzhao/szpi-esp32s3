/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Services - HID 报告构造（键盘 / 鼠标 / 消费类控制）
 */

#include "svc_bt_hid_report.h"
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "svc.bt.hid";

static svc_bt_hid_report_map_t *svc_bt_hid_dev_rpt_tbl;
static uint8_t svc_bt_hid_dev_rpt_tbl_len;

static svc_bt_hid_report_map_t *svc_bt_hid_dev_rpt_by_id(uint8_t id, uint8_t type)
{
    svc_bt_hid_report_map_t *rpt = svc_bt_hid_dev_rpt_tbl;

    for (uint8_t i = svc_bt_hid_dev_rpt_tbl_len; i > 0; i--, rpt++) {
        if (rpt->id == id && rpt->type == type && rpt->mode == protocol_mode) {
            return rpt;
        }
    }

    return NULL;
}

void svc_bt_hid_dev_register_reports(uint8_t num_reports, svc_bt_hid_report_map_t *p_report)
{
    svc_bt_hid_dev_rpt_tbl = p_report;
    svc_bt_hid_dev_rpt_tbl_len = num_reports;
    return;
}

void svc_bt_hid_dev_send_report(esp_gatt_if_t gatts_if, uint16_t conn_id,
                                    uint8_t id, uint8_t type, uint8_t length, uint8_t *data)
{
    svc_bt_hid_report_map_t *p_rpt;

    // get att handle for report
    if ((p_rpt = svc_bt_hid_dev_rpt_by_id(id, type)) != NULL) {
        // if notifications are enabled
        ESP_LOGD(TAG, "%s(), send the report, handle = %d", __func__, p_rpt->handle);
        esp_ble_gatts_send_indicate(gatts_if, conn_id, p_rpt->handle, length, data, false);
    }

    return;
}

void svc_bt_hid_consumer_build_report(uint8_t *buffer, consumer_cmd_t cmd)
{
    if (!buffer) {
        ESP_LOGE(TAG, "%s(), the buffer is NULL, hid build report failed.", __func__);
        return;
    }

    switch (cmd) {
        case HID_CONSUMER_CHANNEL_UP:
            HID_CC_RPT_SET_CHANNEL(buffer, HID_CC_RPT_CHANNEL_UP);
            break;

        case HID_CONSUMER_CHANNEL_DOWN:
            HID_CC_RPT_SET_CHANNEL(buffer, HID_CC_RPT_CHANNEL_DOWN);
            break;

        case HID_CONSUMER_VOLUME_UP:
            HID_CC_RPT_SET_VOLUME_UP(buffer);
            break;

        case HID_CONSUMER_VOLUME_DOWN:
            HID_CC_RPT_SET_VOLUME_DOWN(buffer);
            break;

        case HID_CONSUMER_MUTE:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_MUTE);
            break;

        case HID_CONSUMER_POWER:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_POWER);
            break;

        case HID_CONSUMER_RECALL_LAST:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_LAST);
            break;

        case HID_CONSUMER_ASSIGN_SEL:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_ASSIGN_SEL);
            break;

        case HID_CONSUMER_PLAY:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_PLAY);
            break;

        case HID_CONSUMER_PAUSE:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_PAUSE);
            break;

        case HID_CONSUMER_RECORD:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_RECORD);
            break;

        case HID_CONSUMER_FAST_FORWARD:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_FAST_FWD);
            break;

        case HID_CONSUMER_REWIND:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_REWIND);
            break;

        case HID_CONSUMER_SCAN_NEXT_TRK:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_SCAN_NEXT_TRK);
            break;

        case HID_CONSUMER_SCAN_PREV_TRK:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_SCAN_PREV_TRK);
            break;

        case HID_CONSUMER_STOP:
            HID_CC_RPT_SET_BUTTON(buffer, HID_CC_RPT_STOP);
            break;

        default:
            break;
    }

    return;
}
