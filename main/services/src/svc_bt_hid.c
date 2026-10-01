/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - BLE HID 设备（NET-006）实现
 *
 * HID 协议本体在 svc_bt_hid_dev.c / svc_bt_hid_send.c / svc_bt_hid_report.c
 * （许可见各文件头部）。本文件只做胶水：注册回调、同意配对请求、把 App 侧动作
 * 转成 HID 报告。
 *
 * 注意：GATTS 回调由 svc_bt 注册，事件转发进来（同时注册会互相覆盖）。
 */

#include "svc_common.h"
#include "svc_bt_hid.h"
#include "svc_bt_hid_int.h"
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "svc_bt_hid_send.h"
#include "svc_bt_hid_dev.h"
#include "esp_timer.h"

static const char *TAG = "svc.bt.hid";

static bool s_inited = false;
static bool s_registered = false;    /* HID 服务注册完成 */
static bool s_encrypted = false;     /* 配对 / 加密完成，可以发报告 */
static bool s_connected = false;
static uint16_t s_conn_id = 0;

static void hid_event_cb(svc_bt_hid_evt_t event, svc_bt_hid_evt_param_t *param)
{
    switch (event) {
    case SVC_BT_HID_EVENT_REG_FINISH:
        s_registered = (param->init_finish.state == SVC_BT_HID_INIT_OK);
        if (s_registered) {
            ESP_LOGI(TAG, "hid device registered");
        } else {
            ESP_LOGW(TAG, "hid device register failed");
        }
        break;

    case SVC_BT_HID_EVENT_BLE_CONNECT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        svc_bt_note_conn(true, s_conn_id);
        ESP_LOGI(TAG, "connected (conn_id %u)", (unsigned)s_conn_id);
        break;

    case SVC_BT_HID_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "disconnected");
        s_connected = false;
        s_encrypted = false;
        s_conn_id = 0;
        svc_bt_note_conn(false, 0);
        break;

    case SVC_BT_HID_EVENT_BLE_LED_REPORT_WRITE_EVT:
        /* 主机下发的键盘 LED 状态（大小写 / 数字锁），暂只记录 */
        if (param->led_write.length > 0) {
            ESP_LOGD(TAG, "led report 0x%02x", param->led_write.data[0]);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------ 单击（按下 + 延时松开） ------------------------------ */

/* HID 报告是"状态量"：一次点击 = 按下报告 + 30 ms 后的松开报告 */
#define SVC_BT_HID_CLICK_HOLD_MS    30
#define SVC_BT_HID_RELEASE_NONE     0
#define SVC_BT_HID_RELEASE_KEY      1
#define SVC_BT_HID_RELEASE_CONSUMER 2

static esp_timer_handle_t s_release_timer = NULL;
static uint8_t s_release_kind = SVC_BT_HID_RELEASE_NONE;
static uint8_t s_release_usage = 0;

static void click_release_cb(void *arg)
{
    (void)arg;

    /* 一次性把状态读出来：定时器回调与点击方并发，避免 kind/usage 错配 */
    uint8_t kind = s_release_kind;
    uint8_t usage = s_release_usage;
    uint16_t conn_id = s_conn_id;
    bool connected = s_connected;
    s_release_kind = SVC_BT_HID_RELEASE_NONE;

    if (!connected) return;

    if (kind == SVC_BT_HID_RELEASE_KEY) {
        uint8_t keys[6] = {0};
        svc_bt_hid_send_keyboard(conn_id, 0, keys, 0);
    } else if (kind == SVC_BT_HID_RELEASE_CONSUMER) {
        svc_bt_hid_send_consumer(conn_id, usage, false);
    }
}

static esp_err_t click_schedule(uint8_t kind, uint8_t usage)
{
    if (s_release_timer == NULL) return ESP_ERR_INVALID_STATE;

    /* 先停旧定时器再改状态，最后启动：保证回调读到的 kind/usage 一致 */
    esp_timer_stop(s_release_timer);
    s_release_kind = kind;
    s_release_usage = usage;
    return esp_timer_start_once(s_release_timer, (uint64_t)SVC_BT_HID_CLICK_HOLD_MS * 1000);
}

esp_err_t svc_bt_hid_init(void)
{
    if (s_inited) return ESP_OK;

    /* 先置位再注册协议栈：app 注册是异步的，而 BTC 任务优先级高于调用者，REG 事件
     * 往往在下面几行执行完之前就回调进来；标志没置位时事件会被丢弃，HID 服务便再也
     * 建不起来（现场表现为只打印 initializing，没有 hid svc handle / registered）。 */
    s_inited = true;
    ESP_LOGI(TAG, "initializing (keyboard / mouse / consumer)");

    esp_err_t err = svc_bt_hid_dev_profile_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "profile init failed: %s", esp_err_to_name(err));
        s_inited = false;
        return err;
    }

    err = svc_bt_hid_dev_register_callbacks(hid_event_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register callbacks failed: %s", esp_err_to_name(err));
        s_inited = false;
        return err;
    }

    /* 配对参数：Just Works（无输入无输出）+ 绑定，密钥长度 16 */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    /* 单击辅助用的松开定时器 */
    if (s_release_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = click_release_cb,
            .name = "svc_bt_hid",
        };
        esp_err_t terr = esp_timer_create(&targs, &s_release_timer);
        if (terr != ESP_OK) {
            ESP_LOGW(TAG, "release timer create failed: %s", esp_err_to_name(terr));
            s_inited = false;
            return terr;
        }
    }

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t svc_bt_hid_deinit(void)
{
    if (!s_inited) return ESP_OK;

    svc_bt_hid_dev_profile_deinit();

    if (s_release_timer != NULL) {
        esp_timer_stop(s_release_timer);
        esp_timer_delete(s_release_timer);
        s_release_timer = NULL;
    }
    s_release_kind = SVC_BT_HID_RELEASE_NONE;

    s_inited = false;
    s_registered = false;
    s_encrypted = false;
    s_connected = false;
    s_conn_id = 0;

    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

bool svc_bt_hid_is_ready(void)
{
    return (s_registered && s_encrypted && s_connected);
}

esp_err_t svc_bt_hid_key(uint8_t usage, uint8_t modifier)
{
    if (!svc_bt_hid_is_ready()) return ESP_ERR_INVALID_STATE;

    uint8_t keys[6] = {0};
    uint8_t num = 0;
    if (usage != 0) {
        keys[0] = usage;
        num = 1;
    }

    svc_bt_hid_send_keyboard(s_conn_id, modifier, keys, num);
    return ESP_OK;
}

esp_err_t svc_bt_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy)
{
    if (!svc_bt_hid_is_ready()) return ESP_ERR_INVALID_STATE;

    svc_bt_hid_send_mouse(s_conn_id, buttons, dx, dy);
    return ESP_OK;
}

esp_err_t svc_bt_hid_consumer(uint8_t usage, bool pressed)
{
    if (!svc_bt_hid_is_ready()) return ESP_ERR_INVALID_STATE;

    svc_bt_hid_send_consumer(s_conn_id, usage, pressed);
    return ESP_OK;
}

esp_err_t svc_bt_hid_key_click(uint8_t usage, uint8_t modifier)
{
    if (usage == 0) return ESP_ERR_INVALID_ARG;
    if (!svc_bt_hid_is_ready()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = svc_bt_hid_key(usage, modifier);
    if (err != ESP_OK) return err;

    return click_schedule(SVC_BT_HID_RELEASE_KEY, usage);
}

esp_err_t svc_bt_hid_consumer_click(uint8_t usage)
{
    if (usage == 0) return ESP_ERR_INVALID_ARG;
    if (!svc_bt_hid_is_ready()) return ESP_ERR_INVALID_STATE;

    esp_err_t err = svc_bt_hid_consumer(usage, true);
    if (err != ESP_OK) return err;

    return click_schedule(SVC_BT_HID_RELEASE_CONSUMER, usage);
}

void svc_bt_hid_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                            esp_ble_gatts_cb_param_t *param)
{
    if (!s_inited) return;
    svc_bt_hid_dev_gatts_event(event, gatts_if, param);
}

void svc_bt_hid_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (!s_inited) return;

    switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
        /* 主机发起配对：直接同意（Just Works） */
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            s_encrypted = true;
            ESP_LOGI(TAG, "paired, ready to send reports");
        } else {
            s_encrypted = false;
            ESP_LOGW(TAG, "pairing failed (reason 0x%x)",
                     (unsigned)param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    default:
        break;
    }
}
