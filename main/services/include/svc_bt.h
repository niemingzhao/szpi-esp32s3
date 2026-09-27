/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - 蓝牙 BLE
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BLE 设备名最大长度（含结尾 0） */
#define SVC_BT_NAME_MAX 32
/* 从机广播 / GAP 设备名（手机、电脑在蓝牙列表里看到的就是它）取
 * svc_identity.h 的 svc_identity_bt_name()，形如 SZPI-OS-CA22 */

/** BLE 状态 */
typedef enum {
    SVC_BT_STATE_OFF = 0,      /* 协议栈未启动 */
    SVC_BT_STATE_READY,        /* 已就绪（未广播） */
    SVC_BT_STATE_ADVERTISING,  /* 正在广播（可被连接） */
    SVC_BT_STATE_CONNECTED,    /* 已被中心设备连接 */
} svc_bt_state_t;

/** 扫描到的设备 */
typedef struct {
    uint8_t bda[6];                 /* 蓝牙地址 */
    uint8_t addr_type;              /* 对端地址类型（BLE_ADDR_TYPE_PUBLIC / RANDOM），连接时要原样传入 */
    int8_t rssi;                    /* 信号强度 */
    char name[SVC_BT_NAME_MAX];     /* 广播里的设备名，可能为空 */
} svc_bt_scan_result_t;

/**
 * @brief 启动 BLE 协议栈（Bluedroid + BLE 从机 GATT 服务）
 *
 * 耗时约数百毫秒，且控制器与主机都要内部 RAM（主机的工作队列 / 任务栈不能放 PSRAM），
 * 必须在 services_init() 早期（Wi-Fi 之前）调用，否则会因内部内存不足随机初始化失败。
 * 成功后自动开始广播。
 */
esp_err_t svc_bt_init(void);

/**
 * @brief 当前状态（配套事件 SVC_EVENT_BT_STATE_CHANGED）
 */
svc_bt_state_t svc_bt_get_state(void);

/**
 * @brief 状态名（日志 / 界面用）
 */
const char *svc_bt_state_name(svc_bt_state_t state);

/**
 * @brief 是否已被中心设备连接
 */
bool svc_bt_is_connected(void);

/**
 * @brief 取当前连接的从机对端地址（配套"已连接"状态显示设备名用）
 *
 * @param out 6 字节输出
 * @return true 表示当前有连接且 out 已填写
 */
bool svc_bt_get_peer(uint8_t out[6]);

/**
 * @brief 诊断信息（排查蓝牙问题时用）
 *
 * gap_events 为 0 表示 GAP 回调没有生效，此时收不到广播 / 扫描 / 配对事件。
 */
typedef struct {
    svc_bt_state_t state;   /* 当前状态 */
    bool connected;         /* 已被连接 */
    bool adv_want;          /* 期望广播 */
    bool adv_ready;         /* 广播数据已提交 */
    bool adv_active;        /* 已开始广播 */
    bool scan_ready;        /* 扫描参数已提交 */
    uint32_t gap_events;    /* 收到的 GAP 事件数 */
    uint32_t gap_last;      /* 最近一次 GAP 事件 id */
} svc_bt_diag_t;

esp_err_t svc_bt_get_diag(svc_bt_diag_t *out);

/**
 * @brief 开始广播（从机模式，可被连接）
 */
esp_err_t svc_bt_adv_start(void);

/**
 * @brief 停止广播
 */
esp_err_t svc_bt_adv_stop(void);

/**
 * @brief 断开当前连着的从机对端（HID 主机等）
 *
 * 断开后状态回到 READY 并自动恢复广播；没有连接时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t svc_bt_disconnect_peer(void);

/**
 * @brief 作为中心设备扫描周边 BLE 设备
 *
 * @param[in] duration_s 扫描时长（秒），0 表示默认 10 s
 * 扫描结束发布 SVC_EVENT_BT_SCAN_DONE（数据为 uint32_t 设备数）。
 * 结果里的 bda / addr_type 直接交给 svc_bt_central_connect() 连接（见 svc_bt_central.h）。
 */
esp_err_t svc_bt_scan_start(uint32_t duration_s);

/**
 * @brief 提前结束扫描
 */
esp_err_t svc_bt_scan_stop(void);

/**
 * @brief 读取最近一次扫描结果（按地址去重）
 *
 * @param[out] out   结果数组
 * @param[in]  max   数组容量
 * @param[out] count 实际条数
 */
esp_err_t svc_bt_get_scan_results(svc_bt_scan_result_t *out, size_t max, size_t *count);

/**
 * @brief 通过自定义特征通知中心设备（中心设备需已使能通知）
 */
esp_err_t svc_bt_notify(const void *data, size_t len);

/** 配对提示的类型 */
typedef enum {
    SVC_BT_PAIR_COMPARE = 0,   /* 数字比对：对端和我们显示同一串码，问是否一致 */
    SVC_BT_PAIR_ENTER,         /* 输入配对码：对端显示 6 位码，等我们输进去 */
} svc_bt_pair_mode_t;

/** 配对提示（SVC_EVENT_BT_PAIR_PROMPT 的负载） */
typedef struct {
    svc_bt_pair_mode_t mode;
    uint32_t passkey;          /* COMPARE 时是要比对的 6 位码；ENTER 时无意义 */
} svc_bt_pair_prompt_t;

/**
 * @brief 回应配对提示（收到 SVC_EVENT_BT_PAIR_PROMPT 后调用）
 *
 * @param[in] ok      COMPARE：是否一致；ENTER：true 表示 passkey 有效
 * @param[in] passkey ENTER 时是对端显示的 6 位码；COMPARE 时忽略
 *
 * 配对参数是"安全连接 + MITM + 绑定"，所以对端要么和我们比对同一串码，要么让我们
 * 输入它显示的码；界面由 fw_pairing 统一弹（广播在离开 App 后仍然有效，提示不能只做
 * 在蓝牙 App 里）。
 */
esp_err_t svc_bt_pair_reply(bool ok, uint32_t passkey);

#ifdef __cplusplus
}
#endif
