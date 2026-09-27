/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 蓝牙中心角色（GATT 客户端）实现
 *
 * GATTC 回调由 svc_bt 统一注册再转发（Bluedroid 只允许一个 GATTC 回调），
 * 这里只处理属于自己 app 的事件。连接 / 断开走 esp_ble_gattc_open / close；服务发现走
 * search_service，结果来自 SEARCH_RES / SEARCH_CMPL；读写与订阅先用 UUID 在本地缓存里
 * 查句柄（这几个查询是同步的，不触发事件），再发异步请求，结果由回调上报。
 *
 * 同一时刻只允许一次连接尝试：连不上时 Bluedroid 的 GATTC 连接槽会被未完成的请求占住
 * （日志报 "Max TCB for gatt_if [N] reached."），之后再发起连接全部失败。所以这里用
 * s_connecting 挡住重复请求，并加一个超时兜底（对端不回事件时也要让上层知道结果）。
 */

#include "svc_common.h"
#include "svc_bt_central.h"
#include "svc_bt_central_int.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "svc.bt.central";

#define CENTRAL_APP_ID             1
#define CENTRAL_SVC_MAX            16
#define CENTRAL_CHAR_MAX           12
#define CENTRAL_MTU_DEFAULT        23      /* ATT 默认 MTU */
#define CENTRAL_CCCD_NOTIFY        1       /* CCCD 值：使能通知 */
#define CENTRAL_CCCD_INDICATE      2       /* CCCD 值：使能指示 */
#define CENTRAL_CONNECT_TIMEOUT_US (8 * 1000 * 1000)    /* 连接尝试的超时兜底 */
#define CENTRAL_BOND_MAX           8                    /* 一次最多查这么多绑定记录 */
#define CENTRAL_CB_MAX             4                    /* 事件回调槽个数（App / 脚本各一个） */

static bool s_inited = false;
static esp_gatt_if_t s_if = ESP_GATT_IF_NONE;
static volatile bool s_connected = false;
static volatile bool s_connecting = false;   /* 连接请求已发出、还在等结果 */
static volatile bool s_link_owned = false;   /* 向 s_peer_bda 发起过连接（到链路断开为止） */
static bool s_link_up = false;               /* 已收到 GATTC CONNECT（ACL 已经建起来） */
static uint8_t s_timeout_waits = 0;          /* OPEN 迟迟不来时多等的轮数（最多一次） */
static bool s_enc_requested = false;         /* 本次连接已主动发起过加密 */
static uint16_t s_conn_id = 0;
static uint8_t s_peer_bda[6];
static uint16_t s_mtu = CENTRAL_MTU_DEFAULT;
static esp_timer_handle_t s_conn_timer = NULL;

/* 查绑定记录用的缓冲（GATTC 回调在 BTC 任务里串行执行，用静态缓冲避免占它的栈） */
static esp_ble_bond_dev_t s_bond_list[CENTRAL_BOND_MAX];

static svc_bt_central_cb_t s_cbs[CENTRAL_CB_MAX];
static void *s_cb_users[CENTRAL_CB_MAX];

static SemaphoreHandle_t s_mux = NULL;    /* 保护服务列表 */
static svc_bt_central_service_t s_svcs[CENTRAL_SVC_MAX];
static size_t s_svc_count = 0;
static bool s_discovering = false;

/* 订阅过程：REG_FOR_NOTIFY 成功后写 CCCD，写完由 WRITE_DESCR 收尾 */
static uint16_t s_notify_handle = 0;      /* 已订阅的特征值句柄，0 = 没订阅 */
static uint16_t s_pending_char = 0;
static uint16_t s_pending_cccd = CENTRAL_CCCD_NOTIFY;
static bool s_pending_sub = false;
static bool s_pending_enable = false;

/* ------------------------------ 小工具 ------------------------------ */

static void bda_str(const uint8_t *bda, char *out, size_t len)
{
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void uuid_to_bt(const svc_bt_uuid_t *in, esp_bt_uuid_t *out)
{
    memset(out, 0, sizeof(*out));
    if (in->is_128) {
        out->len = ESP_UUID_LEN_128;
        memcpy(out->uuid.uuid128, in->uuid128, sizeof(in->uuid128));
    } else {
        out->len = ESP_UUID_LEN_16;
        out->uuid.uuid16 = in->uuid16;
    }
}

static void uuid_from_bt(const esp_bt_uuid_t *in, svc_bt_uuid_t *out)
{
    memset(out, 0, sizeof(*out));
    if (in->len == ESP_UUID_LEN_128) {
        out->is_128 = true;
        memcpy(out->uuid128, in->uuid.uuid128, sizeof(out->uuid128));
    } else if (in->len == ESP_UUID_LEN_32) {
        out->uuid16 = (uint16_t)(in->uuid.uuid32 & 0xFFFF);   /* 本项目只用到 16 / 128 位 */
    } else {
        out->uuid16 = in->uuid.uuid16;
    }
}

static size_t central_svc_count(void)
{
    size_t n = 0;

    if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        n = s_svc_count;
        xSemaphoreGive(s_mux);
    }
    return n;
}

static void central_notify(svc_bt_central_evt_t evt, uint16_t handle, uint16_t reason,
                           esp_err_t err, const uint8_t *data, size_t len)
{
    const svc_bt_central_evt_data_t d = {
        .evt = evt, .handle = handle, .reason = reason, .err = err, .data = data, .len = len,
    };

    /* 回调槽有多个（App 与脚本各注册一个），逐个通知 */
    for (size_t i = 0; i < CENTRAL_CB_MAX; i++) {
        if (s_cbs[i] != NULL) s_cbs[i](&d, s_cb_users[i]);
    }
}

static void conn_timer_stop(void)
{
    if (s_conn_timer != NULL) esp_timer_stop(s_conn_timer);
}

static void conn_timer_start(void)
{
    if (s_conn_timer == NULL) return;
    esp_timer_stop(s_conn_timer);
    esp_timer_start_once(s_conn_timer, CENTRAL_CONNECT_TIMEOUT_US);
}

/* 收尾一条还没 OPEN 的连接：ACL 已经建起来就按地址断开，否则取消还挂着的创建请求。
 * 只调 esp_ble_gap_disconnect() 取消不了"还在建的连接"，协议栈会一直挂着这次尝试，
 * 下一次连接会报 BT_GATT: gatt_connect wrong state 并白等一个超时。
 * （esp_ble_gattc_cancel_open() 内部按 is_direct=TRUE 调 BTA_GATTC_CancelOpen，
 * 我们用的直接连接同样适用） */
static void conn_abort_pending(void)
{
    if (s_link_up) {
        esp_ble_gap_disconnect(s_peer_bda);
        return;
    }

    esp_ble_gattc_cancel_open_params_t p = { 0 };
    p.gattc_if = s_if;
    memcpy(p.remote_bda, s_peer_bda, sizeof(p.remote_bda));

    const esp_err_t err = esp_ble_gattc_cancel_open(&p);
    if (err != ESP_OK) ESP_LOGW(TAG, "cancel open failed: %s", esp_err_to_name(err));
}

/* 连接请求挂太久（对端不回事件）：取消掉并告诉上层这次没连上 */
static void conn_timeout_cb(void *arg)
{
    (void)arg;
    if (!s_connecting) return;

    /* ACL 已经建起来、只是 OPEN 还没回来（两者本来就只隔几毫秒，这里是 8 s 边界上的
     * 竞态）：再等一轮，别把已经连上的链路判成失败。最多多等一次 */
    if (s_link_up && s_timeout_waits == 0) {
        s_timeout_waits = 1;
        ESP_LOGW(TAG, "open still pending, wait once more");
        conn_timer_start();
        return;
    }

    char bda[20];
    bda_str(s_peer_bda, bda, sizeof(bda));
    ESP_LOGW(TAG, "connect %s timed out, abort", bda);

    s_connecting = false;
    /* 迟迟没有任何连接事件，说明多半连 ACL 都没建起来：这次尝试作废，
     * 链路的"归属"也一起清掉，别一直挡住以后对端连我们的 HID */
    s_link_owned = false;
    /* 还没走到 OPEN：ACL 在就断开、不在就取消创建请求（否则下一次连接会白等） */
    conn_abort_pending();
    s_link_up = false;
    central_notify(SVC_BT_CENTRAL_EVT_DISCONNECTED, 0, 0, ESP_ERR_TIMEOUT, NULL, 0);
}

/* 对端是否已经绑定过：只对绑定过的对端主动加密（重新加密是即时的，不会弹配对）。
 * 新设备留给对端自己发 Security Request / 访问受保护属性时由协议栈触发 —— 不然
 * "不需要加密的设备"会被强行配对、失败，连带把链路也断掉 */
static bool central_is_bonded(const uint8_t bda[6])
{
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return false;
    if (n > CENTRAL_BOND_MAX) n = CENTRAL_BOND_MAX;

    if (esp_ble_get_bond_device_list(&n, s_bond_list) != ESP_OK) return false;

    for (int i = 0; i < n; i++) {
        if (memcmp(s_bond_list[i].bd_addr, bda, sizeof(s_bond_list[i].bd_addr)) == 0) {
            return true;
        }
    }
    return false;
}

/* 这条链路是否由中心角色主动发起。esp_ble_gattc_open() 也会给每个 GATTS app 发一次
 * CONNECT / DISCONNECT，从机侧（svc_bt 与 HID）靠它把"我们连出去的链路"过滤掉。
 *
 * 不能只看 s_connecting：连接尝试被取消 / 超时后标志就清了，而那条 ACL 的 GATTS
 * CONNECT 可能随后才到，会被从机侧误当成"主机连进来"。所以用 s_link_owned 记到
 * 链路真正断开为止 */
bool svc_bt_central_owns_link(const uint8_t bda[6])
{
    return s_link_owned && memcmp(s_peer_bda, bda, sizeof(s_peer_bda)) == 0;
}

/* 认证 / 加密不足类错误：对端要求加密，但我们还没配对 */
static bool status_needs_encryption(esp_gatt_status_t st)
{
    return st == ESP_GATT_INSUF_AUTHENTICATION || st == ESP_GATT_INSUF_ENCRYPTION ||
           st == ESP_GATT_INSUF_KEY_SIZE;
}

/* 主动给当前对端发起一次加密（本次连接只发一次，避免反复触发）。
 * 配对提示由 svc_bt + fw_pairing 弹出，这里只负责发起 */
static void central_request_encryption(void)
{
    if (s_enc_requested) return;
    s_enc_requested = true;

    ESP_LOGI(TAG, "peer needs encryption, request it");
    const esp_err_t err = esp_ble_set_encryption(s_peer_bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
    if (err != ESP_OK) ESP_LOGW(TAG, "set encryption failed: %s", esp_err_to_name(err));
}

/* 按服务 / 特征 UUID 在本地缓存里找特征值句柄；svc_uuid 传 NULL 表示不限服务范围 */
static esp_err_t central_find_char(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                                   uint16_t *out_handle, uint8_t *out_props)
{
    if (!s_connected || s_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;
    if (char_uuid == NULL || out_handle == NULL) return ESP_ERR_INVALID_ARG;

    uint16_t start = 0x0001;
    uint16_t end = 0xFFFF;

    if (svc_uuid != NULL) {
        esp_bt_uuid_t su;
        uuid_to_bt(svc_uuid, &su);

        esp_gattc_service_elem_t svc;
        uint16_t n = 1;
        if (esp_ble_gattc_get_service(s_if, s_conn_id, &su, &svc, &n, 0) != ESP_GATT_OK || n == 0) {
            return ESP_ERR_NOT_FOUND;
        }
        start = svc.start_handle;
        end = svc.end_handle;
    }

    esp_bt_uuid_t cu;
    uuid_to_bt(char_uuid, &cu);

    esp_gattc_char_elem_t chr;
    uint16_t n = 1;
    if (esp_ble_gattc_get_char_by_uuid(s_if, s_conn_id, start, end, cu, &chr, &n) != ESP_GATT_OK ||
        n == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_handle = chr.char_handle;
    if (out_props != NULL) *out_props = (uint8_t)chr.properties;
    return ESP_OK;
}

/* 找某个特征下面的 CCCD 句柄（订阅时写它） */
static esp_err_t central_find_cccd(uint16_t char_handle, uint16_t *out_handle)
{
    esp_bt_uuid_t cccd_uuid;
    memset(&cccd_uuid, 0, sizeof(cccd_uuid));
    cccd_uuid.len = ESP_UUID_LEN_16;
    cccd_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

    esp_gattc_descr_elem_t descr;
    uint16_t n = 1;
    if (esp_ble_gattc_get_descr_by_char_handle(s_if, s_conn_id, char_handle, cccd_uuid,
                                               &descr, &n) != ESP_GATT_OK || n == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_handle = descr.handle;
    return ESP_OK;
}

/* ------------------------------ 事件 ------------------------------ */

void svc_bt_central_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (!s_inited) return;

    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id != CENTRAL_APP_ID) return;
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "app register failed: %d", param->reg.status);
            return;
        }
        s_if = gattc_if;
        ESP_LOGI(TAG, "ready (if=%d)", (int)gattc_if);
        return;
    }

    if (gattc_if != s_if) return;

    switch (event) {
    case ESP_GATTC_CONNECT_EVT: {
        /* 只有我们自己发起（s_link_owned）的才是中心链路：IDF 文档写明对端连进来的
         * 链路也会报一次 GATTC CONNECT，那种不能算我们的连接 */
        if (!svc_bt_central_owns_link(param->connect.remote_bda)) break;

        char bda[20];
        bda_str(param->connect.remote_bda, bda, sizeof(bda));
        s_conn_id = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(s_peer_bda));
        s_link_up = true;
        ESP_LOGI(TAG, "link up: %s (conn_id=%u)", bda, (unsigned)s_conn_id);

        /* 只对"我们自己发起、且已经绑定过"的对端主动加密（重新加密即时完成、不会弹配对）。
         * 新设备不主动配对：不需要加密的设备会被配对失败连带断链，而需要加密的设备会
         * 自己发 Security Request，或者我们用 central_request_encryption() 在发现 / 读取
         * 被"认证不足"挡下时补一次（见下面 SEARCH_CMPL / READ 的处理）。
         *
         * 只对自己发起的链路发：对端连进来的那条由 svc_bt_hid 负责，两边都发会触发
         * Bluedroid 的 "earlier enc was not done for same device" */
        s_enc_requested = false;
        if (s_connecting && central_is_bonded(s_peer_bda)) {
            central_request_encryption();
        }
        break;
    }

    case ESP_GATTC_OPEN_EVT: {
        /* 只认当前这条链路的结果：换过设备后，旧链路的迟到 OPEN 不要串到新尝试上
         * （否则会把新尝试的 s_connecting / 归属清掉并误报一次失败） */
        if (!s_link_owned ||
            memcmp(param->open.remote_bda, s_peer_bda, sizeof(s_peer_bda)) != 0) {
            break;
        }

        /* 虚拟连接就绪才算连上；失败也要通知上层，界面不能一直停在"连接中" */
        conn_timer_stop();
        s_connecting = false;
        s_link_up = false;

        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "open failed: %d", param->open.status);
            s_connected = false;
            /* 断开事件可能已经先到并把这次尝试收尾了（对端拒绝时先来 rsn=0x100），
             * 用 s_link_owned 判断，避免同一个失败通知两次 */
            if (s_link_owned) {
                s_link_owned = false;
                central_notify(SVC_BT_CENTRAL_EVT_DISCONNECTED, 0, 0, ESP_FAIL, NULL, 0);
            }
            break;
        }

        s_conn_id = param->open.conn_id;
        s_connected = true;
        esp_ble_gattc_send_mtu_req(s_if, s_conn_id);   /* 顺手协商 MTU，失败不影响连接 */
        ESP_LOGI(TAG, "connected");
        central_notify(SVC_BT_CENTRAL_EVT_CONNECTED, 0, 0, ESP_OK, NULL, 0);
        break;
    }

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status == ESP_GATT_OK) {
            s_mtu = param->cfg_mtu.mtu;
            ESP_LOGI(TAG, "mtu %u", (unsigned)s_mtu);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT: {
        /* 只给自己的那条链路收尾（对端连进来的链路也会报到 GATTC） */
        if (!s_link_owned ||
            memcmp(param->disconnect.remote_bda, s_peer_bda, sizeof(s_peer_bda)) != 0) {
            break;
        }

        char bda[20];
        bda_str(param->disconnect.remote_bda, bda, sizeof(bda));
        ESP_LOGW(TAG, "disconnected: %s (reason 0x%x)", bda, (unsigned)param->disconnect.reason);

        const bool was_connected = s_connected;
        const uint16_t reason = (uint16_t)param->disconnect.reason;

        s_link_owned = false;
        s_connected = false;
        s_connecting = false;
        s_link_up = false;
        s_enc_requested = false;
        conn_timer_stop();
        s_discovering = false;
        s_notify_handle = 0;
        s_pending_sub = false;

        if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
            s_svc_count = 0;
            memset(s_svcs, 0, sizeof(s_svcs));
            xSemaphoreGive(s_mux);
        }

        /* 链路还没建立就断开（对端拒绝连接时先来 rsn=0x100）：按"连接失败"上报，
         * 免得界面把没连上的设备说成"已断开"；已建立的链路断开才带原因码 */
        central_notify(SVC_BT_CENTRAL_EVT_DISCONNECTED, 0,
                       was_connected ? reason : 0,
                       was_connected ? ESP_OK : ESP_FAIL, NULL, 0);
        break;
    }

    case ESP_GATTC_SEARCH_RES_EVT:
        if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
            if (s_svc_count < CENTRAL_SVC_MAX) {
                svc_bt_central_service_t *s = &s_svcs[s_svc_count++];
                memset(s, 0, sizeof(*s));
                uuid_from_bt(&param->search_res.srvc_id.uuid, &s->uuid);
                s->start_handle = param->search_res.start_handle;
                s->end_handle = param->search_res.end_handle;
            }
            xSemaphoreGive(s_mux);
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        s_discovering = false;
        ESP_LOGI(TAG, "discover done: %u service(s)", (unsigned)central_svc_count());

        /* 发现被"认证不足"挡下：对端要求加密，补一次主动加密（提示会弹出来）；
         * 配对成功后由 App 重新发现一次 */
        if (status_needs_encryption(param->search_cmpl.status)) {
            central_request_encryption();
        }

        central_notify(SVC_BT_CENTRAL_EVT_DISCOVER_DONE, 0, 0,
                       (param->search_cmpl.status == ESP_GATT_OK) ? ESP_OK : ESP_FAIL, NULL, 0);
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        /* 读被挡下也补一次加密（有些对端只在读受保护属性时才暴露"需要配对"） */
        if (status_needs_encryption(param->read.status)) {
            central_request_encryption();
        }

        central_notify(SVC_BT_CENTRAL_EVT_READ, param->read.handle, 0,
                       (param->read.status == ESP_GATT_OK) ? ESP_OK : ESP_FAIL,
                       param->read.value, param->read.value_len);
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        central_notify(SVC_BT_CENTRAL_EVT_WRITE_DONE, param->write.handle, 0,
                       (param->write.status == ESP_GATT_OK) ? ESP_OK : ESP_FAIL, NULL, 0);
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        /* 订阅时写 CCCD 的收尾 */
        if (s_pending_sub) {
            s_pending_sub = false;
            const esp_err_t err = (param->write.status == ESP_GATT_OK) ? ESP_OK : ESP_FAIL;
            if (err == ESP_OK) s_notify_handle = s_pending_enable ? s_pending_char : 0;
            ESP_LOGI(TAG, "subscribe %s", s_pending_enable ? "on" : "off");
            central_notify(SVC_BT_CENTRAL_EVT_SUBSCRIBED, s_pending_char, 0, err, NULL, 0);
        }
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        /* 注册成功后再写 CCCD 打开通知 / 指示 */
        if (s_pending_sub && s_pending_enable) {
            uint16_t cccd = 0;

            if (param->reg_for_notify.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "register notify failed: %d", param->reg_for_notify.status);
                s_pending_sub = false;
                central_notify(SVC_BT_CENTRAL_EVT_SUBSCRIBED, s_pending_char, 0, ESP_FAIL, NULL, 0);
            } else if (central_find_cccd(s_pending_char, &cccd) != ESP_OK) {
                ESP_LOGW(TAG, "no cccd for handle %u", (unsigned)s_pending_char);
                s_pending_sub = false;
                central_notify(SVC_BT_CENTRAL_EVT_SUBSCRIBED, s_pending_char, 0,
                               ESP_ERR_NOT_FOUND, NULL, 0);
            } else {
                const uint16_t v = s_pending_cccd;
                esp_ble_gattc_write_char_descr(s_if, s_conn_id, cccd, sizeof(v), (uint8_t *)&v,
                                               ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            }
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (s_notify_handle != 0 && param->notify.handle == s_notify_handle) {
            central_notify(SVC_BT_CENTRAL_EVT_NOTIFY, param->notify.handle, 0, ESP_OK,
                           param->notify.value, param->notify.value_len);
        }
        break;

    default:
        break;
    }
}

/* -------------------------------- API -------------------------------- */

esp_err_t svc_bt_central_init(void)
{
    if (s_inited) return ESP_OK;

    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
        if (s_mux == NULL) return ESP_ERR_NO_MEM;
    }

    if (s_conn_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = conn_timeout_cb,
            .name = "bt_ctrl_conn",
        };
        if (esp_timer_create(&targs, &s_conn_timer) != ESP_OK) {
            s_conn_timer = NULL;
            ESP_LOGW(TAG, "connect timer create failed");
        }
    }

    s_inited = true;

    esp_err_t err = esp_ble_gattc_app_register(CENTRAL_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "app register failed: %s", esp_err_to_name(err));
        s_inited = false;
        return err;
    }

    ESP_LOGI(TAG, "registering GATTC app");
    return ESP_OK;
}

esp_err_t svc_bt_central_connect(const uint8_t bda[6], uint8_t addr_type)
{
    if (bda == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;   /* app 还没注册完 */
    if (s_connected || s_connecting) return ESP_ERR_INVALID_STATE;

    char str[20];
    bda_str(bda, str, sizeof(str));

    /* 地址类型必须与广播里的一致：手机多用随机地址，传错会连不上 */
    memcpy(s_peer_bda, bda, sizeof(s_peer_bda));
    s_link_owned = true;
    s_connecting = true;
    s_link_up = false;
    s_timeout_waits = 0;

    /* 中心角色只做"安全连接 + 绑定"（不要 MITM）：这里连的是任意设备，要求 MITM 会让
     * 不支持它的消费设备配对失败，协议栈随后把链路断掉（表现为"连上又掉、刷 BTM 错误"）。
     * HID 从机角色仍用 MITM（主机连进来时由 svc_bt.c 的 gatts_cb 设回），配对提示照旧 */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));

    esp_err_t err = esp_ble_gattc_open(s_if, (uint8_t *)bda, (esp_ble_addr_type_t)addr_type, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s failed: %s", str, esp_err_to_name(err));
        s_connecting = false;
        s_link_owned = false;               /* 没起链路就没有归属，别挡住以后对端连我们 */
        return err;
    }

    conn_timer_start();
    ESP_LOGI(TAG, "connecting to %s (addr_type=%u)", str, (unsigned)addr_type);
    return ESP_OK;
}

esp_err_t svc_bt_central_disconnect(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    /* 连接请求还挂着（还没 OPEN）：按地址取消，并让上层立刻收到收尾事件 */
    if (s_connecting) {
        s_connecting = false;
        s_link_owned = false;       /* 这次尝试作废，迟到的 CONNECT / OPEN 都别再认 */
        conn_abort_pending();
        s_link_up = false;
        conn_timer_stop();
        central_notify(SVC_BT_CENTRAL_EVT_DISCONNECTED, 0, 0, ESP_OK, NULL, 0);
        return ESP_OK;
    }

    if (!s_connected || s_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_ble_gattc_close(s_if, s_conn_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "close failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool svc_bt_central_is_connected(void)
{
    return s_connected;
}

esp_err_t svc_bt_central_get_status(svc_bt_central_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->connected = s_connected;
    out->connecting = s_connecting;
    memcpy(out->peer_bda, s_peer_bda, sizeof(out->peer_bda));
    out->mtu = s_mtu;
    out->service_count = central_svc_count();
    return ESP_OK;
}

esp_err_t svc_bt_central_discover(void)
{
    if (!s_connected || s_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;
    if (s_discovering) return ESP_ERR_INVALID_STATE;

    if (s_mux != NULL && xSemaphoreTake(s_mux, portMAX_DELAY) == pdTRUE) {
        s_svc_count = 0;
        memset(s_svcs, 0, sizeof(s_svcs));
        xSemaphoreGive(s_mux);
    }

    s_discovering = true;

    esp_err_t err = esp_ble_gattc_search_service(s_if, s_conn_id, NULL);
    if (err != ESP_OK) {
        s_discovering = false;
        ESP_LOGW(TAG, "search service failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t svc_bt_central_get_services(svc_bt_central_service_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL || max == 0) return ESP_ERR_INVALID_ARG;
    if (s_mux == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mux, portMAX_DELAY) != pdTRUE) return ESP_ERR_INVALID_STATE;

    const size_t n = (s_svc_count < max) ? s_svc_count : max;
    memcpy(out, s_svcs, n * sizeof(svc_bt_central_service_t));
    *count = n;

    xSemaphoreGive(s_mux);
    return ESP_OK;
}

esp_err_t svc_bt_central_get_chars(const svc_bt_central_service_t *svc,
                                   svc_bt_central_char_t *out, size_t max, size_t *count)
{
    if (svc == NULL || out == NULL || count == NULL || max == 0) return ESP_ERR_INVALID_ARG;
    if (!s_connected || s_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;

    /* 特征比一次能放下的多时按 offset 继续取 */
    size_t m = 0;
    uint16_t offset = 0;

    while (m < max) {
        esp_gattc_char_elem_t elems[CENTRAL_CHAR_MAX];
        uint16_t n = CENTRAL_CHAR_MAX;

        if (esp_ble_gattc_get_all_char(s_if, s_conn_id, svc->start_handle, svc->end_handle,
                                       elems, &n, offset) != ESP_GATT_OK || n == 0) {
            break;
        }

        for (uint16_t i = 0; i < n && m < max; i++) {
            memset(&out[m], 0, sizeof(out[m]));
            uuid_from_bt(&elems[i].uuid, &out[m].uuid);
            out[m].handle = elems[i].char_handle;
            out[m].properties = (uint8_t)elems[i].properties;
            m++;
        }

        offset = (uint16_t)(offset + n);
        if (n < CENTRAL_CHAR_MAX) break;
    }

    *count = m;
    return (m > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t svc_bt_central_read(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid)
{
    uint16_t handle = 0;

    esp_err_t err = central_find_char(svc_uuid, char_uuid, &handle, NULL);
    if (err != ESP_OK) return err;

    err = esp_ble_gattc_read_char(s_if, s_conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t svc_bt_central_write(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                               const void *data, size_t len, bool with_response)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > 0xFFFF) return ESP_ERR_INVALID_SIZE;

    uint16_t handle = 0;

    esp_err_t err = central_find_char(svc_uuid, char_uuid, &handle, NULL);
    if (err != ESP_OK) return err;

    err = esp_ble_gattc_write_char(s_if, s_conn_id, handle, (uint16_t)len, (uint8_t *)data,
                                   with_response ? ESP_GATT_WRITE_TYPE_RSP
                                                 : ESP_GATT_WRITE_TYPE_NO_RSP,
                                   ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t svc_bt_central_subscribe(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                                   bool enable)
{
    if (!s_connected || s_if == ESP_GATT_IF_NONE) return ESP_ERR_INVALID_STATE;
    if (s_pending_sub) return ESP_ERR_INVALID_STATE;   /* 上一次订阅还没收尾 */

    uint16_t handle = 0;
    uint8_t props = 0;

    esp_err_t err = central_find_char(svc_uuid, char_uuid, &handle, &props);
    if (err != ESP_OK) return err;

    s_pending_char = handle;
    s_pending_enable = enable;
    /* 优先使能通知；只支持指示的特征用 0x0002 */
    s_pending_cccd = (props & SVC_BT_CHAR_PROP_NOTIFY) ? CENTRAL_CCCD_NOTIFY : CENTRAL_CCCD_INDICATE;

    if (enable) {
        if ((props & (SVC_BT_CHAR_PROP_NOTIFY | SVC_BT_CHAR_PROP_INDICATE)) == 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        /* 注册成功（REG_FOR_NOTIFY）后再写 CCCD = 1 / 2 */
        s_pending_sub = true;
        err = esp_ble_gattc_register_for_notify(s_if, s_peer_bda, handle);
        if (err != ESP_OK) {
            s_pending_sub = false;
            ESP_LOGW(TAG, "register notify failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    /* 关订阅：先注销，再把 CCCD 写 0 */
    esp_ble_gattc_unregister_for_notify(s_if, s_peer_bda, handle);

    uint16_t cccd = 0;
    if (central_find_cccd(handle, &cccd) != ESP_OK) return ESP_ERR_NOT_FOUND;

    s_pending_sub = true;
    const uint16_t v = 0;
    err = esp_ble_gattc_write_char_descr(s_if, s_conn_id, cccd, sizeof(v), (uint8_t *)&v,
                                         ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        s_pending_sub = false;
        ESP_LOGW(TAG, "write cccd failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t svc_bt_central_register_cb(svc_bt_central_cb_t cb, void *user)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;

    /* 同一个 (cb, user) 重复注册只算一次，避免事件被重复投递 */
    for (size_t i = 0; i < CENTRAL_CB_MAX; i++) {
        if (s_cbs[i] == cb && s_cb_users[i] == user) return ESP_OK;
    }

    for (size_t i = 0; i < CENTRAL_CB_MAX; i++) {
        if (s_cbs[i] == NULL) {
            s_cbs[i] = cb;
            s_cb_users[i] = user;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t svc_bt_central_unregister_cb(svc_bt_central_cb_t cb, void *user)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < CENTRAL_CB_MAX; i++) {
        if (s_cbs[i] == cb && s_cb_users[i] == user) {
            s_cbs[i] = NULL;
            s_cb_users[i] = NULL;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void svc_bt_central_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (param == NULL) return;

    switch (event) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        /* 只认当前中心链路的对端：从机那条链路的配对由 svc_bt_hid 处理 */
        if (!s_link_owned ||
            memcmp(param->ble_security.auth_cmpl.bd_addr, s_peer_bda,
                   sizeof(s_peer_bda)) != 0) {
            return;
        }

        const bool ok = (param->ble_security.auth_cmpl.success != 0);
        ESP_LOGI(TAG, "auth %s (reason 0x%x)", ok ? "done" : "failed",
                 (unsigned)param->ble_security.auth_cmpl.fail_reason);
        central_notify(SVC_BT_CENTRAL_EVT_AUTH_DONE, 0, 0,
                       ok ? ESP_OK : ESP_FAIL, NULL, 0);
        break;
    }

    default:
        break;
    }
}
