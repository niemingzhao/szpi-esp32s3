/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 蓝牙中心角色（GATT 客户端）
 *
 * 与从机角色共存：svc_bt 的广播 + 自定义 GATT 服务、svc_bt_hid 的键鼠是 GATT 服务端，
 * 这里是对端设备的 GATT 客户端（BLE 允许同时保持两条链路，见 CONFIG_BT_ACL_CONNECTIONS）。
 *
 * 所有操作都是异步的：函数返回成功只表示请求交给了协议栈，结果通过回调上报，回调在
 * Bluedroid 的 BTC 任务里执行（栈小），里面只适合拷数据、发消息，不要做耗时操作，
 * 更不要直接调 LVGL（界面请自己转发到 LVGL 任务）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 特征属性位（与 BLE 规范一致，上层不依赖 Bluedroid 的类型） */
#define SVC_BT_CHAR_PROP_READ      0x02
#define SVC_BT_CHAR_PROP_WRITE_NR  0x04
#define SVC_BT_CHAR_PROP_WRITE     0x08
#define SVC_BT_CHAR_PROP_NOTIFY    0x10
#define SVC_BT_CHAR_PROP_INDICATE  0x20

/** 16 位 / 128 位 UUID（is_128 为真时用 uuid128，否则用 uuid16） */
typedef struct {
    bool is_128;
    uint16_t uuid16;
    uint8_t uuid128[16];
} svc_bt_uuid_t;

/** 中心角色事件 */
typedef enum {
    SVC_BT_CENTRAL_EVT_CONNECTED,      /* 已连上对端 */
    SVC_BT_CENTRAL_EVT_DISCONNECTED,   /* 连接断开，或连接尝试失败：
                                          err = ESP_OK 表示正常断开（reason 里是断开原因码），
                                          ESP_FAIL 表示连接失败，ESP_ERR_TIMEOUT 表示超时 */
    SVC_BT_CENTRAL_EVT_DISCOVER_DONE,  /* 服务发现完成，可以读服务列表了 */
    SVC_BT_CENTRAL_EVT_READ,           /* 读到特征值（handle / data / len） */
    SVC_BT_CENTRAL_EVT_WRITE_DONE,     /* 写完成 */
    SVC_BT_CENTRAL_EVT_SUBSCRIBED,     /* 订阅开关已生效 */
    SVC_BT_CENTRAL_EVT_NOTIFY,         /* 收到订阅的通知 / 指示（handle / data / len） */
    SVC_BT_CENTRAL_EVT_AUTH_DONE,      /* 配对 / 加密结果：err = ESP_OK 表示成功 */
} svc_bt_central_evt_t;

typedef struct {
    svc_bt_central_evt_t evt;
    uint16_t handle;          /* 特征值句柄 */
    uint16_t reason;          /* 断开原因码（仅 DISCONNECTED 有效） */
    esp_err_t err;            /* ESP_OK，或失败原因 */
    const uint8_t *data;      /* READ / NOTIFY 时有效，只在回调期间 */
    size_t len;
} svc_bt_central_evt_data_t;

typedef void (*svc_bt_central_cb_t)(const svc_bt_central_evt_data_t *evt, void *user);

/** 连接状态 */
typedef struct {
    bool connected;
    bool connecting;           /* 已发出连接请求、还在等结果 */
    uint8_t peer_bda[6];
    uint16_t mtu;              /* 协商后的 ATT MTU */
    size_t service_count;      /* 已发现的服务数 */
} svc_bt_central_status_t;

/** 发现到的服务 */
typedef struct {
    svc_bt_uuid_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
} svc_bt_central_service_t;

/** 发现到的特征 */
typedef struct {
    svc_bt_uuid_t uuid;
    uint16_t handle;           /* 特征值句柄 */
    uint8_t properties;        /* SVC_BT_CHAR_PROP_* 位掩码 */
} svc_bt_central_char_t;

/**
 * @brief 连接对端（bda / addr_type 取自扫描结果，必须原样传入）
 *
 * 地址类型传错会连不上：手机等设备多用随机地址（BLE_ADDR_TYPE_RANDOM）。
 * 同一时刻只允许一次连接尝试：已经连上、或上一次尝试还没结果时返回
 * ESP_ERR_INVALID_STATE；8 s 内没有结果按超时处理（发 DISCONNECTED + ESP_ERR_TIMEOUT）。
 *
 * 已经绑定过的对端会用 esp_ble_set_encryption() 即时重新加密（不弹配对）；新设备不主动
 * 配对，等对端自己发 Security Request，或服务发现 / 读取被"认证不足"挡下时补一次主动
 * 加密 —— 不需要加密的设备因此不会被"强行配对失败"连带断链。配对结果通过
 * SVC_BT_CENTRAL_EVT_AUTH_DONE 上报。
 *
 * 中心角色的配对只用"安全连接 + 绑定"（不要求 MITM，见 svc_bt_central_connect()）；
 * 要求 MITM 会让不支持它的消费设备配对失败、被协议栈断链。
 */
esp_err_t svc_bt_central_connect(const uint8_t bda[6], uint8_t addr_type);

/**
 * @brief 断开当前中心连接
 */
esp_err_t svc_bt_central_disconnect(void);

/**
 * @brief 是否已连上对端
 */
bool svc_bt_central_is_connected(void);

/**
 * @brief 读取连接状态
 */
esp_err_t svc_bt_central_get_status(svc_bt_central_status_t *out);

/**
 * @brief 发现对端的服务（连上后调用；完成后可读服务列表）
 */
esp_err_t svc_bt_central_discover(void);

/**
 * @brief 读取已发现的服务列表
 */
esp_err_t svc_bt_central_get_services(svc_bt_central_service_t *out, size_t max, size_t *count);

/**
 * @brief 列出某个服务下的全部特征
 */
esp_err_t svc_bt_central_get_chars(const svc_bt_central_service_t *svc,
                                   svc_bt_central_char_t *out, size_t max, size_t *count);

/**
 * @brief 读特征值（svc_uuid 传 NULL 表示不限服务范围）
 */
esp_err_t svc_bt_central_read(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid);

/**
 * @brief 写特征值（with_response = false 走 Write Without Response）
 */
esp_err_t svc_bt_central_write(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                               const void *data, size_t len, bool with_response);

/**
 * @brief 订阅 / 取消订阅特征的通知（内部会写 CCCD）
 */
esp_err_t svc_bt_central_subscribe(const svc_bt_uuid_t *svc_uuid, const svc_bt_uuid_t *char_uuid,
                                   bool enable);

/**
 * @brief 注册事件回调（最多 4 个；同一个 (cb, user) 重复注册只算一次）
 */
esp_err_t svc_bt_central_register_cb(svc_bt_central_cb_t cb, void *user);

/**
 * @brief 注销事件回调
 */
esp_err_t svc_bt_central_unregister_cb(svc_bt_central_cb_t cb, void *user);

/**
 * @brief 转发 GAP 事件（由 svc_bt 的 GAP 回调调用，别在别处注册 GAP 回调）
 *
 * 只用它收配对 / 加密结果（ESP_GAP_BLE_AUTH_CMPL_EVT）：Bluedroid 只允许一个 GAP
 * 回调，所以中心角色这边不自己注册，而是借 svc_bt 的转发。
 */
void svc_bt_central_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

#ifdef __cplusplus
}
#endif
