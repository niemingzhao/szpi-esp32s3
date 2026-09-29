/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - OTA（APP-OTA 固件升级）
 *
 * 填 HTTPS 固件地址 → 二次确认 → svc_ota_start()（下载在独立任务里）→ 轮询进度条；
 * 成功后服务自己写 OTA 分区并重启。地址会存进 NVS，下次打开还在。
 * 本地 .bin 升级需要 Services 层再开一个 API（Apps 不能直接碰 esp_ota_*），留到后面。
 */

#include "app_ota.h"
#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.ota";

#define OTA_URL_MAX   160
#define OTA_NS        "sys"
#define OTA_KEY       "ota_url"

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_url = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_kb = NULL;
static lv_timer_t *s_timer = NULL;

static bool s_was_running = false;

/* ------------------------------ 进度 ------------------------------ */

static void timer_cb(lv_timer_t *t)
{
    (void)t;

    bool running = svc_ota_is_running();
    int progress = svc_ota_progress();

    if (running && progress >= 0) {
        fw_ui_progress_set(s_bar, (uint8_t)progress);
        char buf[48];
        snprintf(buf, sizeof(buf), "正在下载 %d%%", progress);
        lv_label_set_text(s_status, buf);
    } else if (s_was_running && !running) {
        lv_label_set_text(s_status, "升级已结束（成功会自动重启）");
    }

    s_was_running = running;
}

/* ------------------------------ 键盘 ------------------------------ */

static void keyboard_toggle(bool show)
{
    if (s_kb == NULL) return;

    lvgl_port_lock(0);
    if (show) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

static void kb_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_READY:
    case LV_EVENT_CANCEL:
        keyboard_toggle(false);
        break;
    default:
        break;
    }
}

static void ta_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) {
        keyboard_toggle(true);
    }
}

/* ------------------------------ 升级 ------------------------------ */

static void confirm_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;

    if (btn != FW_DIALOG_BTN_OK) return;

    const char *url = lv_textarea_get_text(s_url);
    if (url == NULL || strncmp(url, "https://", 8) != 0) {
        lv_label_set_text(s_status, "只允许 https:// 地址");
        return;
    }

    svc_settings_set_str(OTA_NS, OTA_KEY, url);

    if (svc_ota_start(url) != ESP_OK) {
        lv_label_set_text(s_status, "升级启动失败");
        return;
    }

    s_was_running = true;
    fw_ui_progress_set(s_bar, 0);
    lv_label_set_text(s_status, "正在下载 0%");
    ESP_LOGI(TAG, "ota start: %s", url);
}

static void start_cb(lv_event_t *e)
{
    (void)e;

    if (svc_ota_is_running()) {
        fw_ui_toast("正在升级中", 1500);
        return;
    }

    keyboard_toggle(false);
    fw_ui_dialog(NULL, "固件升级", "从该地址下载固件并安装？成功后设备会自动重启。",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, confirm_cb, NULL);
}

static void abort_cb(lv_event_t *e)
{
    (void)e;

    if (!svc_ota_is_running()) {
        fw_ui_toast("当前没有在升级", 1500);
        return;
    }

    svc_ota_abort();
    lv_label_set_text(s_status, "已请求中止");
}

/* ------------------------------ 生命周期 ------------------------------ */

static void *ota_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    char saved[OTA_URL_MAX] = "";
    svc_settings_get_str(OTA_NS, OTA_KEY, saved, sizeof(saved), "");

    s_url = lv_textarea_create(body);
    lv_obj_set_width(s_url, lv_pct(100));
    lv_obj_set_height(s_url, 38);
    lv_textarea_set_one_line(s_url, true);
    lv_textarea_set_max_length(s_url, OTA_URL_MAX - 1);
    lv_textarea_set_placeholder_text(s_url, "https://…/szpi-os.bin");
    if (saved[0] != '\0') lv_textarea_set_text(s_url, saved);
    lv_obj_set_style_text_font(s_url, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_url, fw_theme_color_text_primary(), 0);
    lv_obj_set_style_bg_color(s_url, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(s_url, 1, 0);
    lv_obj_set_style_border_color(s_url, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(s_url, 8, 0);
    lv_obj_add_event_cb(s_url, ta_event_cb, LV_EVENT_FOCUSED, NULL);

    fw_ui_row_btn(body, LV_SYMBOL_DOWNLOAD, "开始升级", start_cb, NULL);
    fw_ui_row_btn(body, LV_SYMBOL_CLOSE, "中止升级", abort_cb, NULL);

    s_status = lv_label_create(body);
    lv_label_set_text(s_status, "填 HTTPS 地址（当前没有在升级）");
    lv_obj_set_style_text_font(s_status, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_status, fw_theme_color_text_secondary(), 0);

    s_bar = fw_ui_progress_bar(body, "升级进度");
    fw_ui_progress_set(s_bar, 0);

    s_kb = lv_keyboard_create(s_root);
    lv_obj_set_size(s_kb, lv_pct(100), 140);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_secondary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_kb, fw_theme_color_bg_card(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, fw_theme_color_text_primary(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_kb, fw_theme_color_border(), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
    lv_keyboard_set_textarea(s_kb, s_url);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();

    s_timer = lv_timer_create(timer_cb, 500, NULL);

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void ota_on_pause(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

static void ota_on_resume(void *ctx)
{
    (void)ctx;
    if (s_timer != NULL) lv_timer_resume(s_timer);
}

static void ota_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_timer != NULL) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_url = NULL;
    s_status = NULL;
    s_bar = NULL;
    s_kb = NULL;
    lvgl_port_unlock();
}

static bool ota_on_back(void *ctx)
{
    (void)ctx;

    if (s_kb != NULL && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) {
        keyboard_toggle(false);
        return true;
    }
    return false;
}

const fw_app_desc_t app_ota_desc = {
    .name = "OTA",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_DOWNLOAD,
    .on_create = ota_on_create,
    .on_pause = ota_on_pause,
    .on_resume = ota_on_resume,
    .on_destroy = ota_on_destroy,
    .on_back = ota_on_back,
};
