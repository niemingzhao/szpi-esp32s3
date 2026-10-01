/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Camera（APP-CAMERA 相机）
 *
 * 预览：每 150 ms 取一帧 RGB565，拷进自己的 PSRAM 缓冲（LVGL 会用这块内存直接绘制，
 * 不能在绘制期间把驱动的帧还回去），再用 lv_image_set_scale 缩到内容区大小。
 * 拍照（拍照时预览暂停）：
 *   1. 用当前预览帧缩半写一张 24 位 BMP 缩略图（LVGL 能解码，图片 App 里可浏览）；
 *   2. 切到 JPEG、取帧、写 /sdcard/DCIM/IMG_<时间>.jpg（主产物，直接给 PC 看）；
 *   3. 切回 RGB565 继续预览。
 * JPEG 那步失败也不丢照片：BMP 已经落盘，界面会提示。
 *
 * 注意：预览帧是 320×240，内容区只有 320×212，所以缩放 224/256（0.875）显示。
 */

#include "app_camera.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.camera";

#define CAM_PREVIEW_MS   150
#define CAM_DIR          "/sdcard/DCIM"
#define CAM_LIST_MAX     8
#define CAM_NAME_MAX     64
#define CAM_PATH_MAX     300

/* 预览固定用 QVGA / RGB565，内容区 320×212，缩放 0.875 显示 */
#define CAM_SIZE         SVC_CAMERA_SIZE_QVGA
#define CAM_PREVIEW_W    320
#define CAM_PREVIEW_H    240

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_info = NULL;
static lv_obj_t *s_list = NULL;
static lv_timer_t *s_timer = NULL;

static lv_image_dsc_t s_dsc;
static uint8_t *s_frame = NULL;         /* 预览帧副本（RGB565，PSRAM） */
static size_t s_frame_cap = 0;
static uint16_t s_fw = 0;
static uint16_t s_fh = 0;

static bool s_capturing = false;
static unsigned s_shot = 0;

/* ------------------------------- BMP 写入 ------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* 把 src（RGB565，src_w × src_h）缩半写成 24 位 BMP（自下而上、BGR） */
static esp_err_t write_bmp_thumb(const char *path, const uint16_t *src, int src_w, int src_h)
{
    const int w = src_w / 2;
    const int h = src_h / 2;
    const size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;
    const size_t size = 54 + row * (size_t)h;

    uint8_t *buf = malloc(size);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    memset(buf, 0, size);

    buf[0] = 'B';
    buf[1] = 'M';
    put32(buf + 2, (uint32_t)size);
    put32(buf + 10, 54);
    put32(buf + 14, 40);
    put32(buf + 18, (uint32_t)w);
    put32(buf + 22, (uint32_t)h);
    put16(buf + 26, 1);
    put16(buf + 28, 24);
    put32(buf + 34, (uint32_t)(row * (size_t)h));

    for (int y = 0; y < h; y++) {
        uint8_t *dst = buf + 54 + row * (size_t)(h - 1 - y);
        const uint16_t *line = src + (size_t)(y * 2) * (size_t)src_w;
        for (int x = 0; x < w; x++) {
            uint16_t px = line[x * 2];
            uint8_t r = (uint8_t)((px >> 11) & 0x1F);
            uint8_t g = (uint8_t)((px >> 5) & 0x3F);
            uint8_t b = (uint8_t)(px & 0x1F);
            dst[x * 3 + 0] = (uint8_t)(b << 3);
            dst[x * 3 + 1] = (uint8_t)(g << 2);
            dst[x * 3 + 2] = (uint8_t)(r << 3);
        }
    }

    esp_err_t err = svc_storage_write(path, buf, size);
    free(buf);
    return err;
}

/* ------------------------------- 列表 ------------------------------- */

static void refresh_list(void)
{
    if (s_list == NULL) return;

    lvgl_port_lock(0);
    lv_obj_clean(s_list);

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(CAM_DIR, &it) != ESP_OK) {
        fw_ui_list_add(s_list, "还没有照片", NULL, NULL);
        lvgl_port_unlock();
        return;
    }

    int shown = 0;
    svc_storage_entry_t *entry;
    while ((entry = svc_storage_iter_next(it)) != NULL && shown < CAM_LIST_MAX) {
        if (entry->is_dir) continue;

        char line[CAM_NAME_MAX + 24];
        snprintf(line, sizeof(line), "%.60s   %u KB", entry->name,
                 (unsigned)((entry->size + 1023) / 1024));
        fw_ui_list_add(s_list, line, NULL, NULL);
        shown++;
    }
    svc_storage_iter_end(it);

    if (shown == 0) {
        fw_ui_list_add(s_list, "还没有照片", NULL, NULL);
    }
    lvgl_port_unlock();
}

/* ------------------------------- 预览 ------------------------------- */

static void preview_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (s_capturing || s_img == NULL || s_frame == NULL) return;

    svc_camera_frame_t f;
    if (svc_camera_capture(&f) != ESP_OK) return;

    if (!f.jpeg && f.len <= s_frame_cap) {
        memcpy(s_frame, f.data, f.len);
        lv_obj_invalidate(s_img);          /* 数据在同一个缓冲里，重绘即可 */
    }

    svc_camera_release();
}

static void update_info(void)
{
    if (s_info == NULL) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "预览 %ux%u   已拍 %u 张", (unsigned)s_fw, (unsigned)s_fh, s_shot);
    lv_label_set_text(s_info, buf);
}

/* ------------------------------- 拍照 ------------------------------- */

/* 切换格式 / 分辨率：svc_camera_open() 在已打开时直接返回 ESP_OK、参数不生效，
 * 所以切换必须先 svc_camera_close() 再 open（新参数才会生效） */
static esp_err_t camera_reopen(svc_camera_format_t fmt)
{
    svc_camera_close();
    return svc_camera_open(fmt, CAM_SIZE);
}

static void capture_cb(lv_event_t *e)
{
    (void)e;

    if (s_capturing) return;
    s_capturing = true;                    /* 预览暂停 */
    fw_ui_toast("拍照中", 1200);

    svc_storage_mkdir(CAM_DIR);            /* 已存在会失败，忽略 */

    char base[CAM_PATH_MAX];
    if (svc_time_is_synced()) {
        char ts[32];
        svc_time_format(svc_time_now(), "%Y%m%d_%H%M%S", ts, sizeof(ts));
        snprintf(base, sizeof(base), "%s/IMG_%s", CAM_DIR, ts);
    } else {
        snprintf(base, sizeof(base), "%s/IMG_%05u", CAM_DIR, (unsigned)svc_time_now() % 100000u);
    }

    /* 1. 缩略图（BMP）：用当前预览帧，LVGL 能解码，图片 App 里能看 */
    bool thumb_ok = false;
    if (s_frame != NULL && s_fw > 0 && s_fh > 0) {
        char bmp[CAM_PATH_MAX + 8];
        snprintf(bmp, sizeof(bmp), "%s.bmp", base);
        thumb_ok = (write_bmp_thumb(bmp, (const uint16_t *)s_frame, s_fw, s_fh) == ESP_OK);
    }

    /* 2. 主产物：JPEG（传感器直接出 JPEG，不需要软件编码器） */
    bool jpg_ok = false;
    if (camera_reopen(SVC_CAMERA_FMT_JPEG) == ESP_OK) {
        svc_camera_frame_t f;
        if (svc_camera_capture(&f) == ESP_OK) {
            if (f.jpeg) {
                char jpg[CAM_PATH_MAX + 8];
                snprintf(jpg, sizeof(jpg), "%s.jpg", base);
                jpg_ok = (svc_storage_write(jpg, f.data, f.len) == ESP_OK);
            }
            svc_camera_release();
        }
    }
    /* 无论 JPEG 这步成败，都切回 RGB565 让预览继续 */
    camera_reopen(SVC_CAMERA_FMT_RGB565);

    if (jpg_ok || thumb_ok) {
        s_shot++;
        fw_ui_toast(jpg_ok ? "已保存 JPEG" : "已保存 BMP 缩略图", 2000);
    } else {
        fw_ui_toast("拍照失败", 2000);
    }

    ESP_LOGI(TAG, "capture %s: jpeg=%d thumb=%d", base, (int)jpg_ok, (int)thumb_ok);

    refresh_list();
    update_info();
    s_capturing = false;                   /* 预览恢复 */
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *camera_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 先占位显示，拿不到摄像头再提示 */
    s_img = lv_image_create(body);
    lv_obj_set_size(s_img, 280, 210);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 0);

    s_info = lv_label_create(body);
    lv_obj_set_style_text_font(s_info, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_info, fw_theme_color_text_secondary(), 0);

    fw_ui_row_btn(body, LV_SYMBOL_IMAGE, "拍照", capture_cb, NULL);

    s_list = fw_ui_list(body, "最近照片");
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);

    lvgl_port_unlock();

    esp_err_t err = camera_reopen(SVC_CAMERA_FMT_RGB565);
    if (err != ESP_OK) {
        fw_ui_toast("摄像头打开失败", 2500);
        if (s_info != NULL) lv_label_set_text(s_info, "摄像头不可用");
        ESP_LOGW(TAG, "camera open failed: %s", esp_err_to_name(err));
        return s_root;
    }

    s_fw = CAM_PREVIEW_W;
    s_fh = CAM_PREVIEW_H;
    s_frame_cap = (size_t)s_fw * s_fh * 2;

    /* 预览缓冲放 PSRAM（> 2 KB 的 malloc 走 PSRAM），不走内部 RAM */
    s_frame = malloc(s_frame_cap);
    if (s_frame == NULL) {
        fw_ui_toast("内存不足", 2000);
        svc_camera_close();
        return s_root;
    }

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;   /* LVGL 9 要求魔数，否则不按新格式解析 */
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    s_dsc.header.w = s_fw;
    s_dsc.header.h = s_fh;
    s_dsc.header.stride = (uint32_t)s_fw * 2;     /* 每行字节数 */
    s_dsc.data_size = (uint32_t)s_frame_cap;
    s_dsc.data = s_frame;

    lvgl_port_lock(0);
    lv_image_set_src(s_img, &s_dsc);
    lv_image_set_scale(s_img, 224);          /* 0.875：320×240 -> 280×210 */
    lv_image_set_antialias(s_img, false);
    lvgl_port_unlock();

    update_info();
    refresh_list();

    s_timer = lv_timer_create(preview_timer_cb, CAM_PREVIEW_MS, NULL);

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void camera_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void camera_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
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
    s_info = NULL;
    s_list = NULL;
    lvgl_port_unlock();

    if (svc_camera_is_open()) {
        svc_camera_close();
    }
    if (s_frame != NULL) {
        free(s_frame);
        s_frame = NULL;
        s_frame_cap = 0;
    }

    ESP_LOGI(TAG, "destroyed");
}

const fw_app_desc_t app_camera_desc = {
    .name = "Camera",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_IMAGE,
    .on_create = camera_on_create,
    .on_pause = camera_on_pause,
    .on_resume = camera_on_resume,
    .on_destroy = camera_on_destroy,
};
