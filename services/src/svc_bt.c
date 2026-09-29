/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 蓝牙 BLE 实现（Bluedroid）
 *
 * 覆盖需求：
 *   NET-007 蓝牙 BLE 扫描（中心）
 *   NET-008 蓝牙 BLE 从机模式（单一自定义 GATT 服务，含可读写/可通知特征）
 *   NET-006 蓝牙 HID 设备模拟（见 svc_bt_hid）
 *
 * 协议栈为 Bluedroid（CONFIG_BT_BLUEDROID_ENABLED=y），只用 BLE 4.2 特性，
 * 控制器初始化前先释放经典蓝牙内存。
 */

#include "svc_common.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "svc_bt_hid_int.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "svc.bt";

#define BT_DEVICE_NAME   "SZPI-OS"
#define BT_APP_ID        0
#define BT_SCAN_MAX      16
#define BT_ATTR_MAX_LEN  128
#define BT_SCAN_DEFAULT_S 10

/* 自定义 GATT 服务与特征（128 位 UUID）：特征可读 / 可写 / 可通知 */
static const uint8_t BT_SERVICE_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0x98,
    0x25, 0x4b, 0xb4, 0x1a, 0x00, 0x01, 0x00, 0x00,
};
static const uint8_t BT_CHAR_UUID[16] = {
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0x98,
    0x25, 0x4b, 0xb4, 0x1a, 0x00, 0x02, 0x00, 0x00,
};

static bool s_inited = false;
static svc_bt_state_t s_state = SVC_BT_STATE_OFF;

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static uint16_t s_service_handle = 0;
static uint16_t s_char_handle = 0;
static uint16_t s_cccd_handle = 0;
static bool s_connected = false;
static bool s_notify_enabled = false;

static bool s_adv_ready = false;    /* 广播数据已提交 */
static bool s_adv_want = false;     /* 期望广播 */
static bool s_adv_active = false;   /* 正在广播 */
static bool s_scan_ready = false;   /* 扫描参数已提交 */
static bool s_scan_pending = false; /* 参数就绪后要启动扫描 */
static uint32_t s_scan_duration_s = BT_SCAN_DEFAULT_S;

/* 诊断：GAP 回调是否真的在工作（0 表示一个 GAP 事件都没收到） */
static uint32_t s_gap_events = 0;
static uint32_t s_gap_last = 0;

/* 启动自检：init 后 2 s 自动打印一次状态，只看串口日志也能判断蓝牙是否在广播 */
#define BT_SELFTEST_DELAY_US  (2 * 1000 * 1000)
static esp_timer_handle_t s_selftest_timer = NULL;

/* 广播启动失败后的重试：控制器可能在某一刻给不出 adv 环境（HCI 0x07 Memory Full），
 * 过一会儿再试往往就成功了（例如对端刚断开、控制器刚回收资源）。 */
#define BT_ADV_RETRY_DELAY_US (3 * 1000 * 1000)
#define BT_ADV_RETRY_MAX      5
static esp_timer_handle_t s_adv_retry_timer = NULL;
static uint8_t s_adv_retry = 0;

static uint8_t s_value[BT_ATTR_MAX_LEN];
static uint16_t s_value_len = 0;

static svc_bt_data_cb_t s_data_cb = NULL;
static void *s_data_cb_user = NULL;

static SemaphoreHandle_t s_mux = NULL;
static svc_bt_scan_result_t s_scan[BT_SCAN_MAX];
static size_t s_scan_count = 0;

/* HID 服务 0x1812（HID over GATT）的 128 位形式（Bluetooth base UUID，[12]/[13] = 0x12/0x18）。
 * 注意：esp_ble_gap_config_adv_data() 里有一句 `if (service_uuid_len & 0xf) return
 * ESP_ERR_INVALID_ARG;` —— 只接受 16 字节整数倍的列表，直接塞 2 字节的 16 位 UUID 会被拒收，
 * 广播数据永远配不上（官方 HID 例程也是用 16 字节形式）。 */
static uint8_t s_adv_service_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x03c0,               /* HID Generic */
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(s_adv_service_uuid),
    .p_service_uuid = s_adv_service_uuid,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x0020,                                  /* 20 ms */
    .adv_int_max = 0x0040,                                  /* 40 ms */
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x0050,                                /* 50 ms */
    .scan_window = 0x0030,                                  /* 30 ms */
    .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
};

/* ------------------------------- 状态 ------------------------------- */

static void bt_set_state(svc_bt_state_t state)
{
    if (s_state == state) return;

    s_state = state;
    svc_event_bus_publish(SVC_EVENT_BT_STATE_CHANGED, &s_state, sizeof(s_state));
    ESP_LOGI(TAG, "state -> %d", (int)state);
}

const char *svc_bt_state_name(svc_bt_state_t state)
{
    switch (state) {
    case SVC_BT_STATE_OFF: return "off";
    case SVC_BT_STATE_READY: return "ready";
    case SVC_BT_STATE_ADVERTISING: return "advertising";
    case SVC_BT_STATE_CONNECTED: return "connected";
    default: return "unknown";
    }
}

/* 期望广播但还没在广播时启动广播（已连接 / 数据未提交时不动） */
static void bt_adv_sync(void)
{
    if (!s_inited || !s_adv_ready || !s_adv_want) return;
    if (s_connected || s_adv_active) return;

    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start adv request failed: %s", esp_err_to_name(err));
        return;
    }

    /* 这里**不能**乐观置位：控制器可能拒收（例如 HCI 0x07 Memory Full），
     * 真正的结果由 ESP_GAP_BLE_ADV_START_COMPLETE_EVT 确认。 */
}

/* 广播启动失败后的延迟重试（由 ADV_START_COMPLETE 的失败分支安排） */
static void bt_adv_retry_cb(void *arg)
{
    (void)arg;

    if (!s_inited || !s_adv_want || s_connected || s_adv_active) return;

    if (s_adv_retry >= BT_ADV_RETRY_MAX) {
        ESP_LOGW(TAG, "adv retry gave up after %d attempt(s)", (int)s_adv_retry);
        return;
    }

    s_adv_retry++;
    ESP_LOGI(TAG, "adv retry %d/%d", (int)s_adv_retry, (int)BT_ADV_RETRY_MAX);
    bt_adv_sync();
}

static void bt_adv_retry_schedule(void)
{
    if (s_adv_retry_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = bt_adv_retry_cb,
            .name = "svc_bt_adv_rt",
        };
        if (esp_timer_create(&targs, &s_adv_retry_timer) != ESP_OK) {
            s_adv_retry_timer = NULL;
            return;
        }
    }

    esp_timer_stop(s_adv_retry_timer);
    esp_timer_start_once(s_adv_retry_timer, BT_ADV_RETRY_DELAY_US);
}

/* 连接状态由本服务的 GATTS 回调与 HID 回调共同维护（同一条链路） */
void svc_bt_note_conn(bool connected, uint16_t conn_id)
{
    if (connected) {
        if (s_connected) return;            /* 多个 app / 多个回调会重复通知，幂等处理 */
        s_conn_id = conn_id;
        s_connected = true;
        s_notify_enabled = false;
        s_adv_active = false;               /* 连接后控制器自动停止广播 */
        bt_set_state(SVC_BT_STATE_CONNECTED);
        return;
    }

    if (!s_connected) return;               /* 重复的断开通知 */

    s_connected = false;
    s_notify_enabled = false;
    s_adv_retry = 0;                        /* 断开后重新给足重试次数 */
    bt_set_state(SVC_BT_STATE_READY);
    bt_adv_sync();                          /* 断开后自动恢复广播 */
}

/* ------------------------------ 扫描 ------------------------------ */

/* 从一段广播数据里取设备名（0x08 短名 / 0x09 全名） */
static void bt_parse_name(const uint8_t *adv, uint8_t adv_len, char *out, size_t out_len)
{
    out[0] = '\0';

    uint8_t i = 0;
    while (i + 1 < adv_len) {
        uint8_t field_len = adv[i];
        if (field_len == 0) break;
        if ((uint16_t)i + 1 + field_len > adv_len) break;

        uint8_t type = adv[i + 1];
        if (type == 0x08 || type == 0x09) {
            size_t n = (size_t)(field_len - 1);
            if (n > out_len - 1) n = out_len - 1;
            memcpy(out, &adv[i + 2], n);
            out[n] = '\0';
            return;
        }
        i = (uint8_t)(i + field_len + 1);
    }
}

static void bt_on_scan_result(const esp_ble_gap_cb_param_t *param)
{
    const struct ble_scan_result_evt_param *r = &param->scan_rst;
    if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    char name[SVC_BT_NAME_MAX];
    bt_parse_name(r->ble_adv, r->adv_data_len, name, sizeof(name));
    if (name[0] == '\0' && r->scan_rsp_len > 0) {
        bt_parse_name(r->ble_adv + r->adv_data_len, r->scan_rsp_len, name, sizeof(name));
    }

    if (s_mux == NULL || xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return;

    size_t idx = s_scan_count;
    for (size_t i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan[i].bda, r->bda, sizeof(s_scan[i].bda)) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < BT_SCAN_MAX) {
        if (idx == s_scan_count) {
            memset(&s_scan[idx], 0, sizeof(s_scan[idx]));
            memcpy(s_scan[idx].bda, r->bda, sizeof(s_scan[idx].bda));
            s_scan_count++;
        }
        s_scan[idx].rssi = (int8_t)r->rssi;
        if (name[0] != '\0') {
            strncpy(s_scan[idx].name, name, SVC_BT_NAME_MAX - 1);
            s_scan[idx].name[SVC_BT_NAME_MAX - 1] = '\0';
        }
        ESP_LOGD(TAG, "found %02x:%02x:%02x:%02x:%02x:%02x rssi=%d '%s'",
                 r->bda[0], r->bda[1], r->bda[2], r->bda[3], r->bda[4], r->bda[5],
                 (int)r->rssi, s_scan[idx].name);
    }

    xSemaphoreGive(s_mux);
}

static size_t bt_scan_count(void)
{
    size_t n = 0;

    if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        n = s_scan_count;
        xSemaphoreGive(s_mux);
    }
    return n;
}

/* ------------------------------- GAP ------------------------------- */

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    s_gap_events++;
    s_gap_last = (uint32_t)event;

    /* HID 需要处理配对 / 加密事件 */
    svc_bt_hid_gap_event(event, param);

    /* 诊断：扫描结果太频繁，其余事件按 debug 级别打一行（需要时开日志级别看） */
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) {
        ESP_LOGD(TAG, "gap event %d", (int)event);
    }

    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        if (param->adv_data_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "adv data set failed: %d", param->adv_data_cmpl.status);
            break;
        }
        s_adv_ready = true;
        bt_adv_sync();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_adv_active = true;
            s_adv_retry = 0;
            bt_set_state(SVC_BT_STATE_ADVERTISING);
        } else {
            /* 常见于控制器给不出 adv 环境（HCI 0x07 Memory Full）→ 过一会儿再试 */
            ESP_LOGW(TAG, "adv start failed: %d, will retry", param->adv_start_cmpl.status);
            s_adv_active = false;
            if (!s_connected) bt_set_state(SVC_BT_STATE_READY);
            bt_adv_retry_schedule();
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_adv_active = false;
        if (!s_connected) bt_set_state(SVC_BT_STATE_READY);
        break;

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        s_scan_ready = true;
        if (s_scan_pending) {
            s_scan_pending = false;
            esp_ble_gap_start_scanning(s_scan_duration_s);
        }
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "scan start failed: %d", param->scan_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "scanning %u s", (unsigned)s_scan_duration_s);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        bt_on_scan_result(param);
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
        uint32_t count = (uint32_t)bt_scan_count();
        ESP_LOGI(TAG, "scan done: %u device(s)", (unsigned)count);
        svc_event_bus_publish(SVC_EVENT_BT_SCAN_DONE, &count, sizeof(count));
        break;
    }

    default:
        break;
    }
}

/* ------------------------------ GATTS ------------------------------ */

static void gatts_add_char(void)
{
    esp_bt_uuid_t char_uuid;
    memset(&char_uuid, 0, sizeof(char_uuid));
    char_uuid.len = ESP_UUID_LEN_128;
    memcpy(char_uuid.uuid.uuid128, BT_CHAR_UUID, sizeof(BT_CHAR_UUID));

    esp_err_t err = esp_ble_gatts_add_char(s_service_handle, &char_uuid,
                                           ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                           ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE |
                                               ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                           NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add char failed: %s", esp_err_to_name(err));
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    /* HID 设备（若已启用）只处理属于它自己 app 的事件 */
    svc_bt_hid_gatts_event(event, gatts_if, param);

    /* 只处理本服务注册的 app（连接 / 断开事件会被每个 app 各回调一次，按 gatts_if 过滤，
     * HID 那边的通知由 svc_bt_hid 转发给 svc_bt_note_conn） */
    if ((event == ESP_GATTS_CREATE_EVT || event == ESP_GATTS_ADD_CHAR_EVT ||
         event == ESP_GATTS_ADD_CHAR_DESCR_EVT || event == ESP_GATTS_WRITE_EVT ||
         event == ESP_GATTS_READ_EVT || event == ESP_GATTS_CONNECT_EVT ||
         event == ESP_GATTS_DISCONNECT_EVT) && gatts_if != s_gatts_if) {
        return;
    }

    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.app_id != BT_APP_ID) return;
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "app register failed: %d", param->reg.status);
            break;
        }

        s_gatts_if = gatts_if;

        esp_err_t nerr = esp_ble_gap_set_device_name(BT_DEVICE_NAME);
        if (nerr != ESP_OK) {
            ESP_LOGW(TAG, "set device name failed: %s", esp_err_to_name(nerr));
        }

        esp_err_t aerr = esp_ble_gap_config_adv_data(&s_adv_data);
        if (aerr != ESP_OK) {
            ESP_LOGW(TAG, "config adv data failed: %s", esp_err_to_name(aerr));
        } else {
            /* 不等 ADV_DATA_SET_COMPLETE 事件：它只是"已提交"的通知，写入与开启走同一条
             * BTA / BTU 单队列。事件在某些情况下收不到，若只靠它就会永远不广播。 */
            s_adv_ready = true;
            bt_adv_sync();
        }

        esp_gatt_srvc_id_t srvc_id;
        memset(&srvc_id, 0, sizeof(srvc_id));
        srvc_id.is_primary = true;
        srvc_id.id.inst_id = 0;
        srvc_id.id.uuid.len = ESP_UUID_LEN_128;
        memcpy(srvc_id.id.uuid.uuid.uuid128, BT_SERVICE_UUID, sizeof(BT_SERVICE_UUID));

        esp_err_t err = esp_ble_gatts_create_service(gatts_if, &srvc_id, 4);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "create service failed: %s", esp_err_to_name(err));
        }
        break;

    case ESP_GATTS_CREATE_EVT:
        if (param->create.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "service create failed: %d", param->create.status);
            break;
        }
        s_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(s_service_handle);
        gatts_add_char();
        break;

    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "add char failed: %d", param->add_char.status);
            break;
        }
        s_char_handle = param->add_char.attr_handle;

        /* 加 CCCD：中心设备通过它使能通知 */
        {
            esp_bt_uuid_t descr_uuid;
            memset(&descr_uuid, 0, sizeof(descr_uuid));
            descr_uuid.len = ESP_UUID_LEN_16;
            descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

            esp_err_t err = esp_ble_gatts_add_char_descr(s_service_handle, &descr_uuid,
                                                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                         NULL, NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "add cccd failed: %s", esp_err_to_name(err));
            }
        }
        break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        if (param->add_char_descr.status == ESP_GATT_OK) {
            s_cccd_handle = param->add_char_descr.attr_handle;
            ESP_LOGI(TAG, "gatt service ready");
        } else {
            ESP_LOGW(TAG, "add descr failed: %d", param->add_char_descr.status);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        svc_bt_note_conn(true, param->connect.conn_id);
        ESP_LOGI(TAG, "connected: %02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.remote_bda[0], param->connect.remote_bda[1],
                 param->connect.remote_bda[2], param->connect.remote_bda[3],
                 param->connect.remote_bda[4], param->connect.remote_bda[5]);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "disconnected (reason 0x%02x)", param->disconnect.reason);
        svc_bt_note_conn(false, 0);
        break;

    case ESP_GATTS_MTU_EVT:
        ESP_LOGD(TAG, "mtu %u", (unsigned)param->mtu.mtu);
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_char_handle) {
            uint16_t len = param->write.len;
            if (len > BT_ATTR_MAX_LEN) len = BT_ATTR_MAX_LEN;
            memcpy(s_value, param->write.value, len);
            s_value_len = len;
            ESP_LOGD(TAG, "write %u byte(s)", (unsigned)len);

            if (s_data_cb != NULL) {
                s_data_cb(s_value, s_value_len, s_data_cb_user);
            }
        } else if (param->write.handle == s_cccd_handle && param->write.len == 2) {
            s_notify_enabled = (param->write.value[0] & 0x01) != 0;
            ESP_LOGI(TAG, "notify %s", s_notify_enabled ? "on" : "off");
        }

        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                        ESP_GATT_OK, NULL);
        }
        break;

    case ESP_GATTS_READ_EVT:
        if (param->read.handle == s_char_handle && param->read.need_rsp) {
            esp_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(rsp));
            rsp.attr_value.handle = s_char_handle;
            rsp.attr_value.len = s_value_len;
            if (s_value_len > 0) {
                memcpy(rsp.attr_value.value, s_value, s_value_len);
            }
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id,
                                        ESP_GATT_OK, &rsp);
        }
        break;

    case ESP_GATTS_CONF_EVT:
        break;

    default:
        break;
    }
}

/* -------------------------------- API -------------------------------- */

/* 启动自检回调：init 后 2 s 打一条状态，便于只看串口日志判断蓝牙是否真的在广播 */
static void bt_selftest_cb(void *arg)
{
    (void)arg;

    svc_bt_diag_t d;
    if (svc_bt_get_diag(&d) != ESP_OK) return;

    ESP_LOGI(TAG, "selftest: state=%s adv(ready=%d active=%d) scan_ready=%d gap_events=%u last=%u",
             svc_bt_state_name(d.state), (int)d.adv_ready, (int)d.adv_active,
             (int)d.scan_ready, (unsigned)d.gap_events, (unsigned)d.gap_last);
    ESP_LOGI(TAG, "selftest: dma-internal heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

esp_err_t svc_bt_init(void)
{
    if (s_inited) return ESP_OK;

    /* 现场诊断：控制器要一块约 30 KB 的连续内部 DMA 内存，主机要内部 RAM 起任务/队列。
     * largest 比 free 更关键 —— free 够但 largest 不够时同样会 Malloc failed。
     * 用 MALLOC_CAP_DMA（= 内部 DMA 可用区）而不是 MALLOC_CAP_INTERNAL：后者把 IRAM 也算进去，
     * largest 会得出比 free 还大的怪值。 */
    ESP_LOGI(TAG, "init: dma-internal heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == NULL) return ESP_ERR_NO_MEM;
    }

    /* 只用 BLE：先把经典蓝牙的内存还给堆 */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return err;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return err;
    }

    esp_err_t cerr = esp_ble_gatts_register_callback(gatts_cb);
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "register gatts cb failed: %s", esp_err_to_name(cerr));
    }
    cerr = esp_ble_gap_register_callback(gap_cb);
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "register gap cb failed: %s", esp_err_to_name(cerr));
    }

    /* 诊断：确认协议栈里真正生效的回调就是我们的（GAP 回调没生效时收不到任何 GAP 事件） */
    ESP_LOGI(TAG, "callbacks: gatts=%p gap=%p match=%d/%d",
             (void *)esp_ble_gatts_get_callback(), (void *)esp_ble_gap_get_callback(),
             (int)(esp_ble_gatts_get_callback() == gatts_cb),
             (int)(esp_ble_gap_get_callback() == gap_cb));

    esp_ble_gatts_app_register(BT_APP_ID);

    cerr = esp_ble_gap_set_scan_params(&s_scan_params);
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "set scan params failed: %s", esp_err_to_name(cerr));
    } else {
        s_scan_ready = true;                /* 同上：不等 SCAN_PARAM_SET_COMPLETE 事件 */
    }

    /* HID 设备（NET-006）：与自定义 GATT 服务共用同一个协议栈与回调 */
    esp_err_t hid_err = svc_bt_hid_init();
    if (hid_err != ESP_OK) {
        ESP_LOGW(TAG, "hid init failed: %s", esp_err_to_name(hid_err));
    }

    s_inited = true;
    s_adv_want = true;                       /* 默认作为从机广播 */
    bt_set_state(SVC_BT_STATE_READY);

    /* 广播数据在 REG_EVT 里已提交（s_adv_ready），这里直接开广播；若 REG_EVT 还没到，
     * 由该处理里的 bt_adv_sync() 补上。 */
    bt_adv_sync();

    ESP_LOGI(TAG, "init done: dma-internal heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /* 启动自检定时器（只创建一次，之后每轮 init 重新计时） */
    if (s_selftest_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = bt_selftest_cb,
            .name = "svc_bt_diag",
        };
        if (esp_timer_create(&targs, &s_selftest_timer) != ESP_OK) {
            s_selftest_timer = NULL;
        }
    }
    if (s_selftest_timer != NULL) {
        esp_timer_stop(s_selftest_timer);
        esp_timer_start_once(s_selftest_timer, BT_SELFTEST_DELAY_US);
    }

    ESP_LOGI(TAG, "initialized (%s)", BT_DEVICE_NAME);
    return ESP_OK;
}

esp_err_t svc_bt_deinit(void)
{
    if (!s_inited) return ESP_OK;

    if (s_selftest_timer != NULL) {
        esp_timer_stop(s_selftest_timer);
    }
    if (s_adv_retry_timer != NULL) {
        esp_timer_stop(s_adv_retry_timer);
    }
    s_adv_retry = 0;

    svc_bt_adv_stop();
    svc_bt_hid_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_inited = false;
    s_adv_ready = false;
    s_adv_want = false;
    s_adv_active = false;
    s_scan_ready = false;
    s_gatts_if = ESP_GATT_IF_NONE;
    s_connected = false;
    bt_set_state(SVC_BT_STATE_OFF);

    ESP_LOGI(TAG, "deinitialized");
    return ESP_OK;
}

svc_bt_state_t svc_bt_get_state(void)
{
    return s_state;
}

bool svc_bt_is_connected(void)
{
    return s_connected;
}

esp_err_t svc_bt_get_diag(svc_bt_diag_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    out->state = s_state;
    out->connected = s_connected;
    out->adv_want = s_adv_want;
    out->adv_ready = s_adv_ready;
    out->adv_active = s_adv_active;
    out->scan_ready = s_scan_ready;
    out->gap_events = s_gap_events;
    out->gap_last = s_gap_last;
    return ESP_OK;
}

esp_err_t svc_bt_adv_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    s_adv_want = true;
    s_adv_retry = 0;                        /* 用户手动开启时重新给足重试次数 */
    bt_adv_sync();
    return ESP_OK;
}

esp_err_t svc_bt_adv_stop(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    s_adv_want = false;
    if (!s_adv_active) return ESP_OK;

    esp_err_t err = esp_ble_gap_stop_advertising();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop adv failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t svc_bt_scan_start(uint32_t duration_s)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        s_scan_count = 0;
        memset(s_scan, 0, sizeof(s_scan));
        xSemaphoreGive(s_mux);
    }

    s_scan_duration_s = (duration_s > 0) ? duration_s : BT_SCAN_DEFAULT_S;

    /* 扫描参数在初始化时已设置；若还没就绪，等就绪回调里再启动 */
    if (!s_scan_ready) {
        s_scan_pending = true;
        return ESP_OK;
    }

    esp_err_t err = esp_ble_gap_start_scanning(s_scan_duration_s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t svc_bt_scan_stop(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!s_scan_ready) return ESP_OK;
    return esp_ble_gap_stop_scanning();
}

esp_err_t svc_bt_get_scan_results(svc_bt_scan_result_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || max == 0) return ESP_ERR_INVALID_ARG;
    if (s_mux == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    size_t n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(svc_bt_scan_result_t));
    *count = n;

    xSemaphoreGive(s_mux);
    return ESP_OK;
}

esp_err_t svc_bt_notify(const void *data, size_t len)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (!s_inited || !s_connected || s_gatts_if == ESP_GATT_IF_NONE || s_char_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > BT_ATTR_MAX_LEN) return ESP_ERR_INVALID_SIZE;
    if (!s_notify_enabled) return ESP_ERR_INVALID_STATE;

    /* 同步更新特征值，这样中心设备主动读也能拿到最新数据 */
    esp_ble_gatts_set_attr_value(s_char_handle, (uint16_t)len, (uint8_t *)data);

    return esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_char_handle,
                                       (uint16_t)len, (uint8_t *)data, false);
}

esp_err_t svc_bt_register_data_cb(svc_bt_data_cb_t cb, void *user)
{
    s_data_cb = cb;
    s_data_cb_user = user;
    return ESP_OK;
}
