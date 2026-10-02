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
    int8_t rssi;                    /* 信号强度 */
    char name[SVC_BT_NAME_MAX];     /* 广播里的设备名，可能为空 */
} svc_bt_scan_result_t;

/** 收到中心设备写入的数据时的回调（在 BT 任务上下文，不要做耗时操作） */
typedef void (*svc_bt_data_cb_t)(const uint8_t *data, size_t len, void *user);

/**
 * @brief 启动 BLE 协议栈（Bluedroid + BLE 从机 GATT 服务）
 *
 * 耗时约数百毫秒，且控制器与主机都要内部 RAM（主机的工作队列 / 任务栈不能放 PSRAM），
 * 必须在 services_init() 早期（Wi-Fi 之前）调用，否则会因内部内存不足随机初始化失败。
 * 成功后自动开始广播（NET-008）。
 */
esp_err_t svc_bt_init(void);

/**
 * @brief 关闭 BLE 协议栈（仅关机前释放用）
 */
esp_err_t svc_bt_deinit(void);

/**
 * @brief 当前状态（配套事件 SVC_EVENT_BT_STATE_CHANGED）
 */
svc_bt_state_t svc_bt_get_state(void);

/**
 * @brief 状态名（日志 / 界面用）
 */
const char *svc_bt_state_name(svc_bt_state_t state);

/**
 * @brief 本机 BLE 设备名（广播里显示给中心设备的名字，界面提示用）
 */
const char *svc_bt_get_name(void);

/**
 * @brief 是否已被中心设备连接
 */
bool svc_bt_is_connected(void);

/**
 * @brief 诊断信息（排查蓝牙问题时用，串口 bt 命令会打印）
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
 * @brief 作为中心设备扫描周边 BLE 设备
 *
 * @param[in] duration_s 扫描时长（秒），0 表示默认 10 s
 * 扫描结束发布 SVC_EVENT_BT_SCAN_DONE（数据为 uint32_t 设备数）。
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

/**
 * @brief 注册/注销"收到写入数据"的回调（注销传 NULL）
 */
esp_err_t svc_bt_register_data_cb(svc_bt_data_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
