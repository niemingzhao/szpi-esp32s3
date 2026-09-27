/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Camera（相机）
 *
 * 整屏预览 + 一个悬浮拍照按钮（打不开摄像头时变刷新按钮）：内容区不留边距，
 * 预览铺满，按钮悬浮在右下角。
 *
 * 取帧放在独立任务里，不在 LVGL 定时器里：svc_camera_capture() 会阻塞等帧，
 * 放在 LVGL 任务里会把触摸、状态栏、事件总线的界面刷新全部饿死（表现为"只剩
 * BOOT 键有反应、系统很卡"）。任务负责 取帧 → 拷贝 → 通知，LVGL 侧只把后端缓冲
 * 拷到前端缓冲再重绘。
 *
 * 预览不做缩放：直接取 320×240 的中间 320×212 行（正好是内容区高度），按前摄
 * 习惯左右镜像；拍照写的还是未镜像的那块 320×212（所见即所得的构图，方向是
 * 真实方向），24 位 BMP 到 /sdcard/DCIM/IMG_<时间>.bmp（约 199 KB，图库 App 能打开，
 * 电脑也能看）。
 *
 * 离开 App（on_pause）就把摄像头关掉：状态栏摄像头图标随之熄灭，也不白白耗电；
 * 再进来（on_show）重新打开。关闭前先让取帧任务从"正在取帧"里退出来，避免把驱动
 * 从它下面抽掉。
 *
 * 为什么不是 JPEG：esp32-camera 的 JPEG 模式 DMA 缓冲在 ll_cam.c 里写死 16 KB，
 * 而 BLE + Wi-Fi 起来后内部最大连续块只有 15 KB，切 JPEG 必然 malloc 失败。
 */

#include "app_camera.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.camera";

#define CAM_DIR          "/sdcard/DCIM"
#define CAM_PATH_MAX     300
#define CAM_TASK_STACK   4096
#define CAM_SHOW_MS      50                  /* LVGL 侧取帧上屏周期（≈20 fps） */

#define CAM_FULL_W       320                 /* 传感器输出 QVGA */
#define CAM_FULL_H       240
#define CAM_VIEW_H       (240 - FW_STATUSBAR_H)   /* 212：整屏预览高度（不缩放） */
#define CAM_CROP_Y       ((CAM_FULL_H - CAM_VIEW_H) / 2)
#define CAM_FULL_BYTES   (CAM_FULL_W * CAM_FULL_H * 2)
#define CAM_VIEW_BYTES   (CAM_FULL_W * CAM_VIEW_H * 2)

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_btn = NULL;          /* 悬浮拍照 / 重试 */
static lv_timer_t *s_timer = NULL;
static TaskHandle_t s_task = NULL;

static lv_image_dsc_t s_dsc;
static uint8_t *s_frame = NULL;         /* 全分辨率（拍照用，PSRAM） */
static uint8_t *s_view_back = NULL;     /* 取帧任务写（已镜像） */
static uint8_t *s_view_front = NULL;    /* LVGL 显示用（只由 LVGL 任务改） */
static volatile bool s_view_new = false;
static volatile bool s_paused = false;
static volatile bool s_stop = false;
static volatile bool s_in_capture = false;   /* 取帧任务正在驱动里取帧 */

static volatile bool s_open = false;    /* 摄像头是否可用 */
static volatile bool s_busy = false;    /* 拍照任务进行中 */

/* 拍照结果：后台任务写，回到 LVGL 任务后读 */
static struct {
    bool bmp;
    size_t size;
    char base[CAM_PATH_MAX];
} s_result;

/* ------------------------------- 小工具 ------------------------------- */

/* 只更新那个悬浮按钮：能用就是拍照图标，不能用就是刷新图标（点了就地重开） */
static void update_btn(void)
{
    lvgl_port_lock(0);

    if (s_btn != NULL) {
        lv_obj_t *icon = lv_obj_get_child(s_btn, 0);
        if (icon != NULL) {
            lv_image_set_src(icon, s_open ? &icon_ui_camera : &icon_ui_refresh);
        }
    }

    lvgl_port_unlock();
}

/* ------------------------------- 取帧任务 ------------------------------- */

/* 中间 CAM_VIEW_H 行按前摄习惯左右镜像（拍照用未镜像的原帧） */
static void copy_view_mirrored(const uint16_t *src, uint16_t *dst)
{
    for (int y = 0; y < CAM_VIEW_H; y++) {
        const uint16_t *s = src + (size_t)y * CAM_FULL_W;
        uint16_t *d = dst + (size_t)y * CAM_FULL_W;
        for (int x = 0; x < CAM_FULL_W; x++) {
            d[x] = s[CAM_FULL_W - 1 - x];
        }
    }
}

/* 独立任务：取帧（会阻塞等帧）→ 拷全分辨率帧（拍照用）→ 拷镜像后的显示帧。
 * 干完等 LVGL 侧把上一帧取走再取下一帧 —— 界面任务永远不等摄像头 */
static void cam_task(void *arg)
{
    (void)arg;

    while (!s_stop) {
        if (s_paused || s_busy || !s_open) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        svc_camera_frame_t f;
        bool got = false;

        s_in_capture = true;
        if (svc_camera_capture(&f) == ESP_OK) {
            if (!f.jpeg && f.len <= CAM_FULL_BYTES) {
                memcpy(s_frame, f.data, f.len);
                got = true;
            }
            svc_camera_release();
        }
        s_in_capture = false;

        if (got) {
            copy_view_mirrored((const uint16_t *)(s_frame + (size_t)CAM_CROP_Y * CAM_FULL_W * 2),
                               (uint16_t *)s_view_back);
            s_view_new = true;
        }

        /* 上一帧还没被显示任务取走就先别覆盖，免得把正在显示的图像写花；
         * 取走之后立刻取下一帧（帧率由显示侧 + 传感器决定，不再人为压到 6.7 fps） */
        for (int i = 0; i < 30 && s_view_new && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!s_view_new || s_img == NULL) return;

    memcpy(s_view_front, s_view_back, CAM_VIEW_BYTES);
    s_view_new = false;
    lv_obj_invalidate(s_img);
}

/* ------------------------------- 摄像头开关 ------------------------------- */

/* 打开摄像头（+ 准备显示缓冲）；已打开且参数相同时是空操作 */
static esp_err_t try_open(void)
{
    esp_err_t err = svc_camera_open(SVC_CAMERA_FMT_RGB565, SVC_CAMERA_SIZE_QVGA);
    if (err != ESP_OK) {
        s_open = false;
        update_btn();
        return err;
    }

    s_open = true;
    update_btn();
    return ESP_OK;
}

/* 让取帧任务从"正在取帧"里退出来，再关硬件（反序会把驱动从它下面抽掉） */
static void hw_release(void)
{
    s_paused = true;
    for (int i = 0; i < 40 && s_in_capture; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    svc_camera_close();
    s_open = false;
}

/* ------------------------------- 拍照 ------------------------------- */

static void capture_done_async(void *arg)
{
    (void)arg;

    if (s_result.bmp) {
        fw_ui_toast("已保存", 2000);
    } else {
        fw_ui_toast("拍照失败", 2000);
    }

    update_btn();
    s_busy = false;

    ESP_LOGI(TAG, "capture %s: bmp=%d size=%u", s_result.base,
             (int)s_result.bmp, (unsigned)s_result.size);
}

/* 拼 BMP + 写卡放独立任务里：200 KB 写进 FAT 不让 LVGL 任务停住 */
static void capture_task(void *arg)
{
    (void)arg;

    if (s_frame != NULL) {
        char bmp[CAM_PATH_MAX + 8];
        snprintf(bmp, sizeof(bmp), "%s.bmp", s_result.base);
        s_result.bmp = (svc_camera_write_bmp(bmp,
                                             (const uint16_t *)(s_frame + (size_t)CAM_CROP_Y * CAM_FULL_W * 2),
                                             CAM_FULL_W, CAM_VIEW_H) == ESP_OK);
        if (s_result.bmp) {
            size_t sz = 0;
            s_result.size = (svc_storage_exists(bmp, &sz) == ESP_OK) ? sz : 0;
        }
    }

    if (lvgl_port_lock(0)) {
        lv_async_call(capture_done_async, NULL);
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void capture_cb(lv_event_t *e)
{
    (void)e;

    if (s_busy) return;

    /* 打不开（或被关掉）时这个按钮是「重试」 */
    if (!s_open) {
        if (try_open() != ESP_OK) {
            fw_ui_toast("摄像头打开失败", 2500);
            ESP_LOGW(TAG, "camera reopen failed");
        }
        return;
    }

    if (svc_time_is_synced()) {
        char ts[32];
        svc_time_format(svc_time_now(), "%Y%m%d_%H%M%S", ts, sizeof(ts));
        snprintf(s_result.base, sizeof(s_result.base), "%s/IMG_%s", CAM_DIR, ts);
    } else {
        snprintf(s_result.base, sizeof(s_result.base), "%s/IMG_%05u",
                 CAM_DIR, (unsigned)svc_time_now() % 100000u);
    }

    s_result.bmp = false;
    s_result.size = 0;

    svc_storage_mkdir(CAM_DIR);            /* 已存在会失败，忽略 */

    s_busy = true;                         /* 取帧任务暂停，s_frame 保持稳定 */
    fw_ui_toast("拍照中…", 1500);

    if (xTaskCreate(capture_task, "app_cam_shot", CAM_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        fw_ui_toast("拍照失败", 2000);
        ESP_LOGE(TAG, "capture task create failed (internal RAM 紧)");
    }
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *camera_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 整屏预览：内容区不留边距 */
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 0, 0);

    s_img = lv_image_create(body);
    lv_obj_set_width(s_img, lv_pct(100));
    lv_obj_set_flex_grow(s_img, 1);

    /* 悬浮拍照按钮：挂在预览对象上，不挂 body —— body 是 flex 容器，
     * 挂它上面会被当成普通行排版、把预览挤小 */
    s_btn = fw_ui_icon_btn(s_img, &icon_ui_camera, NULL, 52, capture_cb, NULL);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -8);

    lvgl_port_unlock();

    /* 三个缓冲都放 PSRAM（> 2 KB 的 malloc 走 PSRAM） */
    s_frame = malloc(CAM_FULL_BYTES);
    s_view_back = malloc(CAM_VIEW_BYTES);
    s_view_front = malloc(CAM_VIEW_BYTES);
    if (s_frame == NULL || s_view_back == NULL || s_view_front == NULL) {
        fw_ui_toast("内存不足", 2000);
        return s_root;
    }

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;   /* LVGL 9 要求魔数，否则不按新格式解析 */
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    s_dsc.header.w = CAM_FULL_W;
    s_dsc.header.h = CAM_VIEW_H;
    s_dsc.header.stride = (uint32_t)CAM_FULL_W * 2;   /* 一行 640 字节，平面连续 */
    s_dsc.data_size = (uint32_t)CAM_VIEW_BYTES;
    s_dsc.data = s_view_front;

    lvgl_port_lock(0);
    lv_image_set_src(s_img, &s_dsc);              /* 尺寸正好等于内容区：1:1 不缩放 */
    lvgl_port_unlock();

    s_stop = false;
    s_paused = false;
    if (xTaskCreate(cam_task, "app_cam_prev", CAM_TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        fw_ui_toast("内存不足", 2000);
        return s_root;
    }

    if (try_open() != ESP_OK) {
        fw_ui_toast("摄像头打开失败", 2500);
        ESP_LOGW(TAG, "camera open failed");
    }

    s_timer = lv_timer_create(preview_timer_cb, CAM_SHOW_MS, NULL);
    update_btn();

    ESP_LOGI(TAG, "created (%dx%d view)", CAM_FULL_W, CAM_VIEW_H);
    return s_root;
}

/* 离开前台：停刷新、停取帧，并把摄像头关掉（状态栏图标随之熄灭） */
static void camera_on_pause(void *ctx)
{
    (void)ctx;

    if (s_timer != NULL) lv_timer_pause(s_timer);
    hw_release();
}

/* 进前台（on_start / on_resume 都挂：首建走前者，回栈走后者）：重开摄像头 */
static void camera_on_show(void *ctx)
{
    (void)ctx;

    if (s_timer != NULL) lv_timer_resume(s_timer);

    s_paused = false;
    if (try_open() != ESP_OK) {
        fw_ui_toast("摄像头打开失败", 2500);
        ESP_LOGW(TAG, "camera reopen failed");
    }
}

static void camera_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_img = NULL;
    s_btn = NULL;
    lvgl_port_unlock();

    /* 先停取帧任务，再关摄像头：它可能正在 fb_get 里，反序会把驱动从下面抽掉 */
    s_stop = true;
    s_paused = true;
    for (int i = 0; i < 100 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    svc_camera_close();
    s_open = false;

    if (s_task == NULL) {
        free(s_frame);
        s_frame = NULL;
        free(s_view_back);
        s_view_back = NULL;
        free(s_view_front);
        s_view_front = NULL;
    } else {
        ESP_LOGW(TAG, "preview task still running, buffers kept");
    }

    ESP_LOGI(TAG, "destroyed");
}

const fw_app_desc_t app_camera_desc = {
    .name = "Camera",
    .title = "相机",
    .icon = &icon_home_camera,
    .on_create = camera_on_create,
    .on_start = camera_on_show,
    .on_pause = camera_on_pause,
    .on_resume = camera_on_show,
    .on_destroy = camera_on_destroy,
};
