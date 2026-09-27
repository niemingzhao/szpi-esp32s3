/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - About（关于本机）
 *
 * 两页，同一个根屏里显隐切换（返回键从"更多信息"回"关于"）：
 *   关于：头部一行是「更多信息」入口，主信息卡是项目名 / 固件版本 / Wi-Fi MAC，
 *         下面一张详情格放芯片、应用版本、编译日期、编译时间、Flash、ESP-IDF 版本；
 *   更多信息：内部内存 / PSRAM / 运行时长 / 复位原因，以及开源协议（弹窗看全文）与
 *         "恢复出厂设置"（二次确认后经 svc_power_request_reboot 重启）。
 *
 * 只读信息来自 svc_sysinfo，App 不直接调 IDF。详情格用 2 列（一格约 140 px），3 列放不下
 * "编译日期 + 10-06"这种组合。
 */

#include "app_about.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.about";

/* 关于页的详情格：2 列 × 3 行（硬件 / 编译 / 软件各一行） */
static const char *const k_cells[6] = {
    "芯片", "应用",
    "编译日期", "编译时间",
    "Flash", "ESP-IDF",
};

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page_main = NULL;
static lv_obj_t *s_page_more = NULL;
static lv_obj_t *s_table = NULL;
static lv_obj_t *s_mac_lb = NULL;
static lv_obj_t *s_ver_lb = NULL;

static lv_obj_t *s_mem_item = NULL;
static lv_obj_t *s_psram_item = NULL;
static lv_obj_t *s_uptime_item = NULL;
static lv_obj_t *s_reset_item = NULL;

static bool s_on_more = false;

/* ------------------------------- 格式化 ------------------------------- */

/* __DATE__ 形如 "Oct  6 2026"（月 3 字母 + 补空格的日 + 年），取成 "10-06" */
static void short_date(const char *date, char *out, size_t len)
{
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    if (date != NULL && strlen(date) >= 6) {
        for (int i = 0; i < 12; i++) {
            if (strncmp(date, mon[i], 3) == 0) {
                const int d = atoi(date + 4);
                if (d > 0 && d <= 31) {
                    snprintf(out, len, "%02d-%02d", i + 1, d);
                    return;
                }
                break;
            }
        }
    }

    snprintf(out, len, "%.5s", (date != NULL) ? date : "--");
}

static void uptime_text(uint32_t sec, char *out, size_t len)
{
    if (sec >= 3600) {
        snprintf(out, len, "%u 小时 %u 分", (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60));
    } else if (sec >= 60) {
        snprintf(out, len, "%u 分 %u 秒", (unsigned)(sec / 60), (unsigned)(sec % 60));
    } else {
        snprintf(out, len, "%u 秒", (unsigned)sec);
    }
}

/* ------------------------------- 刷新 ------------------------------- */

static void refresh(void)
{
    svc_sysinfo_t si;
    if (svc_sysinfo_get(&si) != ESP_OK) return;

    char buf[64];

    if (s_ver_lb != NULL) lv_label_set_text(s_ver_lb, SZPI_OS_VERSION);

    if (s_mac_lb != NULL) {
        snprintf(buf, sizeof(buf), "MAC %s", si.mac);
        lv_label_set_text(s_mac_lb, buf);
    }

    if (s_table != NULL) {
        fw_ui_table_value(s_table, 0, si.chip_model);

        fw_ui_table_value(s_table, 1, (si.app_version != NULL) ? si.app_version : "--");

        short_date(si.build_date, buf, sizeof(buf));
        fw_ui_table_value(s_table, 2, buf);

        snprintf(buf, sizeof(buf), "%.5s", (si.build_time != NULL) ? si.build_time : "--");
        fw_ui_table_value(s_table, 3, buf);

        fw_ui_format_size(buf, sizeof(buf), si.flash_size);
        fw_ui_table_value(s_table, 4, buf);

        fw_ui_table_value(s_table, 5, si.idf_version);
    }

    if (s_mem_item != NULL) {
        fw_ui_format_size(buf, sizeof(buf), si.heap_internal_free);
        fw_ui_list_item_value(s_mem_item, buf);
    }
    if (s_psram_item != NULL) {
        fw_ui_format_size(buf, sizeof(buf), si.heap_psram_free);
        fw_ui_list_item_value(s_psram_item, buf);
    }
    if (s_uptime_item != NULL) {
        uptime_text(si.uptime_s, buf, sizeof(buf));
        fw_ui_list_item_value(s_uptime_item, buf);
    }
    if (s_reset_item != NULL) {
        fw_ui_list_item_value(s_reset_item, si.reset_reason);
    }
}

/* ------------------------------- 开源协议 / 恢复出厂设置 ------------------------------- */

static void license_done_cb(fw_dialog_btn_t btn, void *user)
{
    (void)btn;
    (void)user;
}

static void license_cb(lv_event_t *e)
{
    (void)e;

    fw_ui_dialog(NULL, "开源协议",
                 "Noto Sans SC：SIL Open Font License 1.1\n"
                 "LVGL：MIT License\n"
                 "ESP-IDF：Apache License 2.0\n"
                 "Helix MP3 解码器：RealNetworks 公共许可",
                 FW_DIALOG_BTN_OK, license_done_cb, NULL);
}

static void reboot_cb(lv_timer_t *t)
{
    (void)t;
    svc_power_request_reboot();
}

static void factory_cb(fw_dialog_btn_t btn, void *user)
{
    (void)user;
    if (btn != FW_DIALOG_BTN_OK) return;

    if (svc_settings_factory_reset() != ESP_OK) {
        fw_ui_toast("恢复出厂设置失败", 2000);
        return;
    }

    fw_ui_toast("已恢复出厂设置，正在重启…", 2000);

    /* 留出看提示的时间再重启：交给电源服务，App 不直接调 esp_restart */
    lv_timer_t *t = lv_timer_create(reboot_cb, 1800, NULL);
    if (t != NULL) lv_timer_set_repeat_count(t, 1);
}

static void factory_row_cb(lv_event_t *e)
{
    (void)e;

    fw_ui_dialog(NULL, "恢复出厂设置",
                 "将清除全部设置与 Wi-Fi 凭据，且无法恢复，确定继续？",
                 FW_DIALOG_BTN_OK | FW_DIALOG_BTN_CANCEL, factory_cb, NULL);
}

/* ------------------------------- 翻页 ------------------------------- */

static void more_cb(lv_event_t *e)
{
    (void)e;

    s_on_more = true;
    if (s_page_main != NULL) lv_obj_set_hidden(s_page_main, true);
    if (s_page_more != NULL) lv_obj_set_hidden(s_page_more, false);
    refresh();
}

static void back_cb(lv_event_t *e)
{
    (void)e;

    s_on_more = false;
    if (s_page_main != NULL) lv_obj_set_hidden(s_page_main, false);
    if (s_page_more != NULL) lv_obj_set_hidden(s_page_more, true);
}

/* ------------------------------- 界面 ------------------------------- */

static void *about_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);
    lv_obj_set_scrollable(body, false);

    s_on_more = false;

    /* ---- 关于页 ---- */
    s_page_main = lv_obj_create(body);
    lv_obj_set_width(s_page_main, lv_pct(100));
    lv_obj_set_flex_grow(s_page_main, 1);
    lv_obj_set_scrollable(s_page_main, false);
    lv_obj_set_style_bg_opa(s_page_main, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_main, 0, 0);
    lv_obj_set_style_pad_all(s_page_main, 0, 0);
    lv_obj_set_style_pad_row(s_page_main, 8, 0);
    lv_obj_set_flex_flow(s_page_main, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page_main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 头部一行：更多信息入口（内存 / 运行时长 / 复位原因 / 协议 / 恢复出厂设置） */
    lv_obj_t *head = lv_obj_create(s_page_main);
    lv_obj_set_size(head, lv_pct(100), 28);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    fw_ui_icon_btn(head, &icon_ui_more, "更多信息", 110, more_cb, NULL);

    /* 主信息卡：项目 / 版本 + MAC */
    lv_obj_t *card = fw_ui_hero_card(s_page_main, 64);

    lv_obj_t *line1 = lv_obj_create(card);
    lv_obj_set_size(line1, lv_pct(100), 26);
    lv_obj_set_scrollable(line1, false);
    lv_obj_set_style_bg_opa(line1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line1, 0, 0);
    lv_obj_set_style_pad_all(line1, 0, 0);
    lv_obj_set_flex_flow(line1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(line1, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *name = lv_label_create(line1);
    lv_obj_set_width(name, 210);            /* 右侧给版本号留位（DOT 截断要定宽） */
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(name, SZPI_OS_NAME);
    lv_obj_set_style_text_font(name, fw_asset_font_20(), 0);
    lv_obj_set_style_text_color(name, fw_theme_color_text_primary(), 0);

    s_ver_lb = lv_label_create(line1);
    lv_label_set_text(s_ver_lb, SZPI_OS_VERSION);
    lv_obj_set_style_text_font(s_ver_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_ver_lb, fw_theme_color_accent(), 0);

    s_mac_lb = lv_label_create(card);
    lv_obj_set_width(s_mac_lb, lv_pct(100));
    lv_label_set_long_mode(s_mac_lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_mac_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_mac_lb, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_mac_lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_table = fw_ui_table(s_page_main, 2, 3, k_cells);
    /* "应用"是构建版本号（git 号，可能带 -dirty），格子放不下：这一格改成超长横向滚动 */
    fw_ui_table_value_scroll(s_table, 1, true);

    /* ---- 更多信息页 ---- */
    s_page_more = lv_obj_create(body);
    lv_obj_set_width(s_page_more, lv_pct(100));
    lv_obj_set_flex_grow(s_page_more, 1);
    lv_obj_set_scrollable(s_page_more, false);
    lv_obj_set_style_bg_opa(s_page_more, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page_more, 0, 0);
    lv_obj_set_style_pad_all(s_page_more, 0, 0);
    lv_obj_set_style_pad_row(s_page_more, 8, 0);
    lv_obj_set_flex_flow(s_page_more, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page_more, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_hidden(s_page_more, true);

    lv_obj_t *mhead = lv_obj_create(s_page_more);
    lv_obj_set_size(mhead, lv_pct(100), 28);
    lv_obj_set_scrollable(mhead, false);
    lv_obj_set_style_bg_opa(mhead, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mhead, 0, 0);
    lv_obj_set_style_pad_all(mhead, 0, 0);
    lv_obj_set_style_pad_column(mhead, 8, 0);
    lv_obj_set_flex_flow(mhead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mhead, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 返回走状态栏返回键 / BOOT 单击（on_back 回"关于"页），页内不再放返回按钮 */
    lv_obj_t *title = lv_label_create(mhead);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_text(title, "更多信息");
    lv_obj_set_style_text_font(title, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(title, fw_theme_color_text_primary(), 0);

    lv_obj_t *list = fw_ui_list(s_page_more, NULL);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);

    s_mem_item = fw_ui_list_add_value(list, "内部内存", "-", NULL, NULL);
    s_psram_item = fw_ui_list_add_value(list, "PSRAM", "-", NULL, NULL);
    s_uptime_item = fw_ui_list_add_value(list, "运行时长", "-", NULL, NULL);
    s_reset_item = fw_ui_list_add_value(list, "复位原因", "-", NULL, NULL);
    fw_ui_list_add_icon(list, &icon_ui_file_text, "开源协议", "查看", license_cb, NULL);
    fw_ui_list_add_icon(list, &icon_ui_trash, "恢复出厂设置", NULL, factory_row_cb, NULL);

    refresh();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (%s)", SZPI_OS_VERSION);
    return s_root;
}

/* 只读页：回到前台时数值可能已经变了（运行时长 / 内存），重新读一遍 */
static void about_on_resume(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    refresh();
    lvgl_port_unlock();
}

static void about_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_page_main = NULL;
    s_page_more = NULL;
    s_table = NULL;
    s_mac_lb = NULL;
    s_ver_lb = NULL;
    s_mem_item = NULL;
    s_psram_item = NULL;
    s_uptime_item = NULL;
    s_reset_item = NULL;
    lvgl_port_unlock();

    s_on_more = false;
}

/* "更多信息"页开着时，返回键先回"关于"页 */
static bool about_on_back(void *ctx)
{
    (void)ctx;

    if (!s_on_more) return false;

    back_cb(NULL);
    return true;
}

const fw_app_desc_t app_about_desc = {
    .name = "About",
    .title = "关于本机",
    .icon = &icon_home_about,
    .on_create = about_on_create,
    .on_start = about_on_resume,
    .on_resume = about_on_resume,
    .on_destroy = about_on_destroy,
    .on_back = about_on_back,
};
