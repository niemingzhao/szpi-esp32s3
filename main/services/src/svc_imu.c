/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Services - IMU 运动 / 姿态 / 摇晃 / 抬手实现
 *
 * QMI8658 的中断引脚未引出，用软件轮询代替中断：每 50 ms 读一次，
 * 运动 / 朝向 / 摇晃 / 抬手成立时发布对应事件。运动与朝向只在"变化"时发布，
 * 摇晃与抬手自带冷却，避免刷事件总线。
 *
 * 判定阈值从 svc_settings 的 imu 命名空间读一次并缓存（key 见 imu_load_cfg()），
 * 读取失败或取值越界时回落到与最初编译期常量完全一致的默认值。
 */

#include "svc_common.h"
#include "periph_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "svc.imu";

#define IMU_POLL_MS                 50
#define IMU_TASK_STACK              3072
#define IMU_GRAVITY_MS2             9.80665f

/* NVS 命名空间与阈值默认值（与存储的整数同域：比例类按 100 倍整数存） */
#define IMU_NVS_NS                  "imu"
#define IMU_DEF_MOTION_ACC_PCT      15       /* 15 -> 0.15，合加速度偏离 1 g 的百分比 */
#define IMU_DEF_MOTION_GYRO_DPS     60       /* 单位 °/s */
#define IMU_DEF_ORIENT_AXIS_G       60       /* 60 -> 0.60，重力在水平轴上的投影占比 */
#define IMU_DEF_SHAKE_COUNT         3        /* 窗口内越阈次数 */
#define IMU_DEF_SHAKE_WINDOW_MS     1000
#define IMU_DEF_PICKUP_STILL_MS     1000     /* 抬手判定前要求的"平放且静止"时长 */
#define IMU_DEF_PICKUP_TILT_PCT     87       /* 87 -> 0.87，重力在 z 轴上的占比小于它算"屏幕立起来了" */
#define IMU_DEF_PICKUP_HOLD_MS      200      /* "立起来"要保持这么久才算数（滤掉磕碰引起的晃动） */

/* 合理上限：配置超出即视为离谱，回落默认值（下限统一为 1） */
#define IMU_ACC_PCT_MAX             500      /* 5.00 */
#define IMU_GYRO_DPS_MAX            2000
#define IMU_ORIENT_AXIS_MAX         100      /* 1.00 */
#define IMU_PICKUP_TILT_MAX         100      /* 1.00 */
#define IMU_SHAKE_COUNT_MAX         8
#define IMU_WINDOW_MS_MAX           10000
#define IMU_HOLD_MS_MAX             5000

/* 固定行为常量（属于算法而非灵敏度，不暴露到 NVS） */
#define IMU_SHAKE_COOLDOWN_MS       2000
#define IMU_PICKUP_COOLDOWN_MS      3000
#define IMU_PICKUP_FLAT_NZ          0.92f    /* |z 轴占比| 高于它算"平放"（约 23° 以内） */

/*
 * 阈值缓存：任务启动时读一次，50 ms 轮询里不再碰 NVS。
 * 比例类（acc_pct / orient_axis_g / pickup_tilt_pct）存的是 100 倍整数，读出来换算成浮点。
 */
static float s_motion_acc_g;        /* |合加速度 - 1 g| 的运动阈值（m/s²） */
static float s_motion_gyro_dps;
static float s_orient_axis;         /* 归一化比例，0~1 */
static float s_pickup_tilt;         /* 归一化比例，0~1 */
static uint32_t s_shake_window_ms;
static uint8_t s_shake_count;
static uint32_t s_pickup_still_ms;
static uint32_t s_pickup_hold_ms;

static bool s_started = false;
static volatile bool s_moving = false;
static volatile svc_imu_orientation_t s_orientation = SVC_IMU_ORIENTATION_PORTRAIT;

/*
 * 摇晃检测状态：shake_window_ms 内合加速度"由下向上越过运动阈值"的次数达到
 * shake_count（默认 3）即判定摇晃，发布一次后进入 IMU_SHAKE_COOLDOWN_MS 冷却。
 *
 * 取舍：50 ms 轮询下 1 s 窗口只有约 20 个采样点，快速抖动的一个往返可能整个落在
 * 两次采样之间；因此这里只统计"越阈次数"，不还原波形，灵敏度靠 shake_count 与
 * shake_window_ms 调整。ts 是环形时间戳，长度为编译期上限。
 */
typedef struct {
    uint32_t ts[IMU_SHAKE_COUNT_MAX];
    uint8_t head;                 /* 下一个写入位置 */
    uint8_t count;                /* 当前窗口内的越阈次数（<= IMU_SHAKE_COUNT_MAX） */
    bool acc_over;                /* 上一采样点的合加速度是否越阈，用于取上升沿 */
    uint32_t cooldown_until;
} imu_shake_t;

/*
 * 抬手（拿起设备）判定：
 *   1) 设备"平放且静止"（|z 占比| ≥ IMU_PICKUP_FLAT_NZ、无运动）达 pickup_still_ms → 待命；
 *   2) 待命期间屏幕立起来（|z 占比| ≤ pickup_tilt_pct，即倾斜约 30° 以上）并保持
 *      pickup_hold_ms → 发布一次 SVC_EVENT_IMU_PICKUP，随后冷却 IMU_PICKUP_COOLDOWN_MS。
 *
 * 取舍：
 *   - 不用加速度阶跃：手轻轻拿起时合加速度的阶跃远小于 0.35 g，根本摸不到。
 *   - 不用纯运动 / 摇晃：放在桌上被碰到就会亮屏。
 *   - 待命之后不再重新累计"平放静止"：缓慢拿起时中途会有停顿与抖动，若那时把状态
 *     打回原形并重取基准，就再也测不到"立起来"了（这正是前几版慢拿不亮的原因）。
 *   - 磕碰只会让设备在平放姿态附近晃一下，到不了倾斜阈值、也保持不住保持时长。
 */
typedef struct {
    bool flat_rest;               /* 当前处于"平放且静止"段 */
    bool armed;                   /* 平放静止够久，等一次"立起来" */
    uint32_t flat_since;          /* 该段的起点 */
    uint32_t tilt_since;          /* "立起来"的起点，0 = 当前未立起 */
    uint32_t cooldown_until;
} imu_pickup_t;

static uint32_t imu_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static float imu_acc_mag(const periph_imu_data_t *d)
{
    return sqrtf(d->acc_x * d->acc_x + d->acc_y * d->acc_y + d->acc_z * d->acc_z);
}

static float imu_gyr_mag(const periph_imu_data_t *d)
{
    return sqrtf(d->gyr_x * d->gyr_x + d->gyr_y * d->gyr_y + d->gyr_z * d->gyr_z);
}

static bool imu_is_moving(const periph_imu_data_t *d)
{
    return (fabsf(imu_acc_mag(d) - IMU_GRAVITY_MS2) > s_motion_acc_g) ||
           (imu_gyr_mag(d) > s_motion_gyro_dps);
}

/* 看重力主要落在哪个水平轴上；屏幕平放（z 占优）时保持上一次结果 */
static svc_imu_orientation_t imu_orientation_of(const periph_imu_data_t *d,
                                                svc_imu_orientation_t last)
{
    const float acc_mag = imu_acc_mag(d);
    if (acc_mag <= 0.01f) return last;

    const float nx = fabsf(d->acc_x) / acc_mag;
    const float ny = fabsf(d->acc_y) / acc_mag;
    const float nz = fabsf(d->acc_z) / acc_mag;

    if (nz >= s_orient_axis) return last;

    if (ny >= nx) {
        return (d->acc_y >= 0.0f) ? SVC_IMU_ORIENTATION_PORTRAIT
                                  : SVC_IMU_ORIENTATION_PORTRAIT_FLIP;
    }
    return (d->acc_x >= 0.0f) ? SVC_IMU_ORIENTATION_LANDSCAPE
                              : SVC_IMU_ORIENTATION_LANDSCAPE_FLIP;
}

/* |重力在 z 轴上的占比|：平放时接近 1，立起来后接近 0 */
static float imu_nz_ratio(const periph_imu_data_t *d)
{
    const float acc_mag = imu_acc_mag(d);
    if (acc_mag <= 0.01f) return 0.0f;      /* 失重等异常读数按"已立起"处理 */
    return fabsf(d->acc_z) / acc_mag;
}

static int32_t imu_cfg_i32(const char *key, int32_t def, int32_t min, int32_t max)
{
    int32_t v = def;
    const esp_err_t err = svc_settings_get_i32(IMU_NVS_NS, key, &v, def);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "setting %s read failed (%s), use default %d",
                 key, esp_err_to_name(err), (int)def);
        return def;
    }
    if (v < min || v > max) {
        ESP_LOGW(TAG, "setting %s out of range (%d), use default %d", key, (int)v, (int)def);
        return def;
    }
    return v;
}

static void imu_load_cfg(void)
{
    const int32_t acc_pct   = imu_cfg_i32("motion_acc_pct",  IMU_DEF_MOTION_ACC_PCT, 1, IMU_ACC_PCT_MAX);
    const int32_t gyro_dps  = imu_cfg_i32("motion_gyro_dps", IMU_DEF_MOTION_GYRO_DPS, 1, IMU_GYRO_DPS_MAX);
    const int32_t orient    = imu_cfg_i32("orient_axis_g",   IMU_DEF_ORIENT_AXIS_G, 1, IMU_ORIENT_AXIS_MAX);
    const int32_t shake_n   = imu_cfg_i32("shake_count",     IMU_DEF_SHAKE_COUNT, 1, IMU_SHAKE_COUNT_MAX);
    const int32_t shake_win = imu_cfg_i32("shake_window_ms", IMU_DEF_SHAKE_WINDOW_MS, IMU_POLL_MS, IMU_WINDOW_MS_MAX);
    const int32_t still_ms  = imu_cfg_i32("pickup_still_ms", IMU_DEF_PICKUP_STILL_MS, IMU_POLL_MS, IMU_WINDOW_MS_MAX);
    const int32_t tilt_pct  = imu_cfg_i32("pickup_tilt_pct", IMU_DEF_PICKUP_TILT_PCT, 1, IMU_PICKUP_TILT_MAX);
    const int32_t hold_ms   = imu_cfg_i32("pickup_hold_ms",  IMU_DEF_PICKUP_HOLD_MS, IMU_POLL_MS, IMU_HOLD_MS_MAX);

    s_motion_acc_g    = (float)acc_pct / 100.0f * IMU_GRAVITY_MS2;
    s_motion_gyro_dps = (float)gyro_dps;
    s_orient_axis     = (float)orient / 100.0f;
    s_pickup_tilt     = (float)tilt_pct / 100.0f;
    s_shake_count     = (uint8_t)shake_n;
    s_shake_window_ms = (uint32_t)shake_win;
    s_pickup_still_ms = (uint32_t)still_ms;
    s_pickup_hold_ms  = (uint32_t)hold_ms;

    ESP_LOGI(TAG, "cfg: acc=%d%% gyro=%d dps orient=%d%% shake=%u/%u ms still=%u ms tilt=%d%% hold=%u ms",
             (int)acc_pct, (int)gyro_dps, (int)orient,
             (unsigned)s_shake_count, (unsigned)s_shake_window_ms,
             (unsigned)s_pickup_still_ms, (int)tilt_pct, (unsigned)s_pickup_hold_ms);
}

static void imu_shake_expire(imu_shake_t *s, uint32_t now, uint32_t window)
{
    /* 时间戳按写入顺序递增，从最旧的一端开始丢出窗口的 */
    while (s->count > 0) {
        const uint8_t oldest =
            (uint8_t)((s->head + IMU_SHAKE_COUNT_MAX - s->count) % IMU_SHAKE_COUNT_MAX);
        if ((uint32_t)(now - s->ts[oldest]) <= window) break;
        s->count--;
    }
}

static void imu_shake_push(imu_shake_t *s, uint32_t now)
{
    s->ts[s->head] = now;
    s->head = (uint8_t)((s->head + 1) % IMU_SHAKE_COUNT_MAX);
    if (s->count < IMU_SHAKE_COUNT_MAX) s->count++;
}

static bool imu_shake_update(imu_shake_t *s, const periph_imu_data_t *d,
                             uint32_t now, uint8_t *out_count)
{
    *out_count = 0;

    const bool over = fabsf(imu_acc_mag(d) - IMU_GRAVITY_MS2) > s_motion_acc_g;

    /* 冷却中仍然跟踪上升沿，避免冷却结束瞬间用陈旧状态误判 */
    if (s->cooldown_until != 0 && (int32_t)(now - s->cooldown_until) < 0) {
        s->acc_over = over;
        return false;
    }

    imu_shake_expire(s, now, s_shake_window_ms);

    bool trigger = false;
    if (over && !s->acc_over) {
        imu_shake_push(s, now);
        if (s->count >= s_shake_count) trigger = true;
    }
    s->acc_over = over;

    if (trigger) {
        *out_count = s->count;
        s->count = 0;
        s->head = 0;
        s->cooldown_until = now + IMU_SHAKE_COOLDOWN_MS;
    }

    return trigger;
}

static bool imu_pickup_update(imu_pickup_t *s, const periph_imu_data_t *d,
                              bool moving, uint32_t now)
{
    const float nz = imu_nz_ratio(d);

    if (s->cooldown_until != 0 && (int32_t)(now - s->cooldown_until) < 0) {
        /* 冷却期：清掉待命，重新从"平放静止"开始计时 */
        s->armed = false;
        s->tilt_since = 0;
        s->flat_rest = (!moving && nz >= IMU_PICKUP_FLAT_NZ);
        if (s->flat_rest) s->flat_since = now;
        return false;
    }

    /* 平放且静止：累计时长，够久就"待命" */
    if (!moving && nz >= IMU_PICKUP_FLAT_NZ) {
        if (!s->flat_rest) {
            s->flat_rest = true;
            s->flat_since = now;
        } else if ((uint32_t)(now - s->flat_since) >= s_pickup_still_ms) {
            s->armed = true;
        }
        s->tilt_since = 0;
        return false;
    }

    /* 离开"平放静止"：不重开计时、也不改基准，只等"立起来" */
    s->flat_rest = false;

    if (s->armed && nz <= s_pickup_tilt) {
        if (s->tilt_since == 0) s->tilt_since = now;
        if ((uint32_t)(now - s->tilt_since) >= s_pickup_hold_ms) {
            s->armed = false;
            s->tilt_since = 0;
            s->cooldown_until = now + IMU_PICKUP_COOLDOWN_MS;
            return true;
        }
    } else {
        s->tilt_since = 0;
    }

    return false;
}

static void imu_task(void *arg)
{
    (void)arg;

    /* 纳入 Task WDT：本任务 50 ms 一圈 */
    if (svc_watchdog_subscribe() != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog subscribe failed");
    }

    imu_load_cfg();

    /* 先探一次：IMU 不在线时后面的轮询都是空转，启动日志里留一条便于定位 */
    periph_imu_data_t probe;
    if (periph_imu_read(&probe) != ESP_OK) {
        ESP_LOGW(TAG, "IMU not responding; motion / pickup detection will stay idle");
    }

    imu_shake_t shake = {0};
    imu_pickup_t pickup = {0};

    bool last_moving = false;
    svc_imu_orientation_t last_orientation = SVC_IMU_ORIENTATION_PORTRAIT;

    while (true) {
        periph_imu_data_t data;

        if (periph_imu_read(&data) == ESP_OK) {
            const uint32_t now = imu_now_ms();
            const bool moving = imu_is_moving(&data);
            const svc_imu_orientation_t orientation = imu_orientation_of(&data, last_orientation);

            s_moving = moving;
            s_orientation = orientation;

            if (moving != last_moving) {
                last_moving = moving;
                /* 只在开始运动时发布，停止运动不发 */
                if (moving) {
                    svc_event_bus_publish(SVC_EVENT_IMU_MOTION, &moving, sizeof(moving));
                }
            }

            if (orientation != last_orientation) {
                last_orientation = orientation;
                svc_event_bus_publish(SVC_EVENT_IMU_ORIENTATION, &orientation, sizeof(orientation));
                ESP_LOGI(TAG, "orientation -> %d", (int)orientation);
            }

            uint8_t shake_count = 0;
            if (imu_shake_update(&shake, &data, now, &shake_count)) {
                /* 负载为本次窗口内统计到的越阈次数（uint8_t） */
                svc_event_bus_publish(SVC_EVENT_IMU_SHAKE, &shake_count, sizeof(shake_count));
                ESP_LOGI(TAG, "shake (%u)", (unsigned)shake_count);
            }

            if (imu_pickup_update(&pickup, &data, moving, now)) {
                /* 抬手事件无负载（NULL/0），订阅方按事件 ID 判断即可 */
                svc_event_bus_publish(SVC_EVENT_IMU_PICKUP, NULL, 0);
                ESP_LOGI(TAG, "pickup");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
        svc_watchdog_feed();
    }
}

esp_err_t svc_imu_init(void)
{
    if (s_started) return ESP_OK;

    if (xTaskCreatePinnedToCore(imu_task, "imu_task", IMU_TASK_STACK, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "initialized (poll %d ms)", IMU_POLL_MS);
    return ESP_OK;
}

bool svc_imu_is_moving(void)
{
    return s_moving;
}

svc_imu_orientation_t svc_imu_get_orientation(void)
{
    return s_orientation;
}

esp_err_t svc_imu_read(svc_imu_data_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;

    /* Services 的对外类型与 Peripherals 的原始类型各一套，避免头文件互相暴露 */
    periph_imu_data_t d;
    esp_err_t err = periph_imu_read(&d);
    if (err != ESP_OK) return err;

    *out = (svc_imu_data_t){
        .acc_x = d.acc_x, .acc_y = d.acc_y, .acc_z = d.acc_z,
        .gyr_x = d.gyr_x, .gyr_y = d.gyr_y, .gyr_z = d.gyr_z,
        .roll = d.roll, .pitch = d.pitch, .yaw = d.yaw,
    };
    return ESP_OK;
}
