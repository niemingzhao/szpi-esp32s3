/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Bluetooth（APP-BT 蓝牙）
 *
 * BLE 从机：显示协议栈状态、开关广播、扫描周边设备，并作为键鼠从机发送 HID 报告。
 * 状态与扫描结果经 svc_event_bus 通知（SVC_EVENT_BT_STATE_CHANGED / SVC_EVENT_BT_SCAN_DONE）。
 *
 * 两页（页面风格见 docs/03-design/02-ui-system.md 12.6，返回键交给 on_back）：
 *   主页：头部（状态文字 + 广播 / 扫描 / 键鼠三个图标按钮）+ 周边设备列表（撑满）
 *   键鼠页：4 列 × 4 行网格，按功能分区：
 *           方向键 / 键盘常用 / 鼠标点击（左中右）/ 鼠标移动（上下左右）
 *
 * 扫描到的设备只作展示：本服务按 PRD NET-002 只做从机（不发起中心连接、不做 GATT 客户端），
 * 所以点设备只给一句提示，不去连接。没广播名字的设备用蓝牙地址显示。
 *
 * HID（NET-009）：svc_bt_init() 已初始化 HID 设备，主机配对后本页的动作才可发送。
 * 注意 HID 报告是"已配对加密"之后才允许发送的（见 svc_bt_hid.h），所以点击时用
 * svc_bt_hid_is_ready() 判定，未就绪只给 Toast 提示、不硬发。
 */

#include "app_bt.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char *TAG = "app.bt";

#define BT_SCAN_SECONDS     5
#define BT_SCAN_MAX         16
/* 扫描事件万一没回来时的兜底（正常 SCAN_DONE 会先到，定时器随即删掉） */
#define BT_SCAN_GUARD_MS    (BT_SCAN_SECONDS * 1000 + 3000)

#define HEADER_H            30
/* 4 列 × 4 行：70×4 + 4×3 = 292 ≤ 296，42×4 + 6×3 = 186 ≤ 188 */
#define CELL_W              70
#define CELL_H              42
#define GRID_GAP_X          4
#define GRID_GAP_Y          6

/* svc_bt_hid.h 只暴露了常用的一部分用法值，其余按 HID 规范补 */
#define HID_KEY_TAB         0x2B
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_SPACE       0x2C

/* 鼠标报告按键位（点击统一为左 / 中 / 右） */
#define HID_MOUSE_BTN_LEFT   0x01
#define HID_MOUSE_BTN_MIDDLE 0x04
#define HID_MOUSE_BTN_RIGHT  0x02

/* 鼠标移动步长（相对坐标，-127 ~ 127） */
#define HID_MOUSE_MOVE_STEP  20

/* 鼠标"单击"先发按下、30 ms 后补发松开，避免主机把按下状态一直保持 */
#define HID_MOUSE_RELEASE_MS 30

enum { PAGE_MAIN = 0, PAGE_HID, PAGE_COUNT };

typedef enum {
    HID_ACT_KEY,           /* 键盘按键（usage 是 HID 用法值） */
    HID_ACT_MOUSE_CLICK,   /* 鼠标点击（usage 是按键位掩码） */
    HID_ACT_MOUSE_MOVE,    /* 鼠标移动（dx / dy） */
} hid_kind_t;

typedef struct {
    const char *name;
    hid_kind_t kind;
    uint8_t usage;
    int8_t dx;
    int8_t dy;
} hid_action_t;

/* 4 列 × 4 行，按功能分区 */
static const hid_action_t HID_ACTIONS[] = {
    /* 方向键 */
    { "方向上",     HID_ACT_KEY,         SVC_BT_HID_KEY_UP_ARROW,    0, 0 },
    { "方向下",     HID_ACT_KEY,         SVC_BT_HID_KEY_DOWN_ARROW,  0, 0 },
    { "方向左",     HID_ACT_KEY,         SVC_BT_HID_KEY_LEFT_ARROW,  0, 0 },
    { "方向右",     HID_ACT_KEY,         SVC_BT_HID_KEY_RIGHT_ARROW, 0, 0 },
    /* 键盘常用 */
    { "Esc",        HID_ACT_KEY,         SVC_BT_HID_KEY_ESCAPE,      0, 0 },
    { "Tab",        HID_ACT_KEY,         HID_KEY_TAB,                0, 0 },
    { "退格",       HID_ACT_KEY,         HID_KEY_BACKSPACE,          0, 0 },
    { "空格",       HID_ACT_KEY,         HID_KEY_SPACE,              0, 0 },
    /* 回车 + 鼠标点击 */
    { "回车",       HID_ACT_KEY,         SVC_BT_HID_KEY_ENTER,       0, 0 },
    { "鼠标左键",   HID_ACT_MOUSE_CLICK, HID_MOUSE_BTN_LEFT,         0, 0 },
    { "鼠标中键",   HID_ACT_MOUSE_CLICK, HID_MOUSE_BTN_MIDDLE,       0, 0 },
    { "鼠标右键",   HID_ACT_MOUSE_CLICK, HID_MOUSE_BTN_RIGHT,        0, 0 },
    /* 鼠标移动 */
    { "鼠标上移",   HID_ACT_MOUSE_MOVE,  0, 0, -HID_MOUSE_MOVE_STEP },
    { "鼠标下移",   HID_ACT_MOUSE_MOVE,  0, 0,  HID_MOUSE_MOVE_STEP },
    { "鼠标左移",   HID_ACT_MOUSE_MOVE,  0, -HID_MOUSE_MOVE_STEP, 0 },
    { "鼠标右移",   HID_ACT_MOUSE_MOVE,  0,  HID_MOUSE_MOVE_STEP, 0 },
};

#define HID_ACTION_COUNT (sizeof(HID_ACTIONS) / sizeof(HID_ACTIONS[0]))

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page[PAGE_COUNT] = { 0 };
static int s_page_cur = PAGE_MAIN;

static lv_obj_t *s_state_icon = NULL;
static lv_obj_t *s_state_lb = NULL;
static lv_obj_t *s_adv_btn = NULL;
static lv_obj_t *s_list = NULL;
static lv_timer_t *s_scan_guard = NULL;
static bool s_scanning = false;
static bool s_scanned = false;
static bool s_foreground = false;

/* ---------------------------------- 状态 ---------------------------------- */

static const char *state_text(svc_bt_state_t st)
{
    switch (st) {
    case SVC_BT_STATE_OFF: return "未启动";
    case SVC_BT_STATE_READY: return "就绪";
    case SVC_BT_STATE_ADVERTISING: return "广播中";
    case SVC_BT_STATE_CONNECTED: return "已连接";
    default: return "未知";
    }
}

static void refresh_state(void)
{
    const svc_bt_state_t st = svc_bt_get_state();
    const bool active = (st == SVC_BT_STATE_ADVERTISING || st == SVC_BT_STATE_CONNECTED);

    if (s_state_lb != NULL) {
        lv_label_set_text(s_state_lb, state_text(st));
    }
    if (s_state_icon != NULL) {
        lv_obj_set_style_image_recolor(s_state_icon,
                                       active ? fw_theme_color_accent()
                                              : fw_theme_color_text_disabled(), 0);
    }
    if (s_adv_btn != NULL) {
        lv_obj_t *icon = lv_obj_get_child(s_adv_btn, 0);
        if (icon != NULL) {
            lv_image_set_src(icon, (st == SVC_BT_STATE_ADVERTISING) ? &icon_ui_pause
                                                                   : &icon_ui_play);
        }
    }
}

/* 本服务只做从机，点设备不去连接（PRD NET-002）；这里顺便告诉用户本机的蓝牙名，
 * 方便到手机 / 电脑上去找它（和配网热点的提示一个意思） */
static void device_cb(lv_event_t *e)
{
    (void)e;

    char msg[96];
    snprintf(msg, sizeof(msg), "仅支持从机模式，请在手机或电脑上搜索并配对本机（%s）",
             svc_bt_get_name());
    fw_ui_toast(msg, 2600);
}

/* 填列表并返回发现的设备数（正在扫描时返回 0） */
static size_t fill_scan_list(void)
{
    if (s_list == NULL) return 0;

    lv_obj_clean(s_list);

    if (s_scanning) {
        fw_ui_list_hint(s_list, "正在扫描…");
        return 0;
    }

    svc_bt_scan_result_t res[BT_SCAN_MAX];
    size_t count = 0;
    if (svc_bt_get_scan_results(res, BT_SCAN_MAX, &count) != ESP_OK) count = 0;

    if (count == 0) {
        fw_ui_list_hint(s_list, s_scanned ? "未发现设备" : "点右上角扫描设备");
        return 0;
    }

    for (size_t i = 0; i < count; i++) {
        char who[SVC_BT_NAME_MAX];
        if (res[i].name[0] != '\0') {
            snprintf(who, sizeof(who), "%s", res[i].name);
        } else {
            /* 没广播名字的设备用蓝牙地址显示 */
            snprintf(who, sizeof(who), "%02X:%02X:%02X:%02X:%02X:%02X",
                     res[i].bda[0], res[i].bda[1], res[i].bda[2],
                     res[i].bda[3], res[i].bda[4], res[i].bda[5]);
        }

        char rssi[20];
        snprintf(rssi, sizeof(rssi), "%d dBm", (int)res[i].rssi);
        lv_obj_t *item = fw_ui_list_add_icon(s_list, &icon_ui_bt, who, rssi, device_cb, NULL);

        /* 和 Wi-Fi 列表同一套信号分色规则 */
        if (res[i].rssi >= -60) {
            fw_ui_list_value_color(item, fw_theme_color_success());
        } else if (res[i].rssi < -78) {
            fw_ui_list_value_color(item, fw_theme_color_warning());
        }
    }
    return count;
}

/* ---------------------------------- 广播 / 扫描 ---------------------------------- */

static void adv_cb(lv_event_t *e)
{
    (void)e;

    const svc_bt_state_t st = svc_bt_get_state();
    if (st == SVC_BT_STATE_ADVERTISING) {
        svc_bt_adv_stop();
    } else if (st == SVC_BT_STATE_READY) {
        svc_bt_adv_start();
    } else if (st == SVC_BT_STATE_CONNECTED) {
        fw_ui_toast("已被连接，广播暂停", 2000);
    } else {
        fw_ui_toast("蓝牙未就绪", 1500);
    }
    refresh_state();
}

/* 扫描收尾（SCAN_DONE 事件与兜底定时器共用）：刷新列表并提示结果。
 * 兜底那条只在事件没回来时才走到，提示内容与正常收尾一致，不出现"超时"字样 */
static void scan_finish(void)
{
    if (s_scan_guard != NULL) {
        lv_timer_delete(s_scan_guard);
        s_scan_guard = NULL;
    }

    s_scanning = false;
    s_scanned = true;

    const size_t count = fill_scan_list();

    char msg[32];
    snprintf(msg, sizeof(msg), "发现 %u 个设备", (unsigned)count);
    fw_ui_toast(msg, 2000);
}

static void scan_guard_cb(lv_timer_t *t)
{
    (void)t;
    s_scan_guard = NULL;             /* 一次性定时器，LVGL 自己删 */
    if (!s_scanning) return;

    ESP_LOGW(TAG, "scan done event missing, finish by timer");
    scan_finish();
}

static void scan_begin(void)
{
    if (s_scanning) return;
    if (svc_bt_scan_start(BT_SCAN_SECONDS) != ESP_OK) {
        fw_ui_toast("扫描失败", 1500);
        return;
    }

    s_scanning = true;
    fill_scan_list();               /* 先显示"正在扫描…" */

    if (s_scan_guard == NULL) {
        s_scan_guard = lv_timer_create(scan_guard_cb, BT_SCAN_GUARD_MS, NULL);
        if (s_scan_guard != NULL) lv_timer_set_repeat_count(s_scan_guard, 1);
    }
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    scan_begin();
}

/* ---------------------------------- 键鼠模拟 ---------------------------------- */

/* 发报告前的统一判定：未连接 / 未配对只提示，不硬发（服务层同样会拒绝） */
static bool hid_guard(void)
{
    if (!svc_bt_is_connected()) {
        fw_ui_toast("未连接，请先配对", 2000);
        return false;
    }
    if (!svc_bt_hid_is_ready()) {
        fw_ui_toast("配对未完成，请稍后重试", 2000);
        return false;
    }
    return true;
}

static void hid_mouse_release_cb(lv_timer_t *t)
{
    (void)t;
    if (svc_bt_hid_is_ready()) {
        svc_bt_hid_mouse(0, 0, 0);
    }
}

static void hid_action_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= HID_ACTION_COUNT) return;
    if (!hid_guard()) return;

    const hid_action_t *a = &HID_ACTIONS[idx];

    esp_err_t err;
    if (a->kind == HID_ACT_KEY) {
        err = svc_bt_hid_key_click(a->usage, 0);
    } else if (a->kind == HID_ACT_MOUSE_CLICK) {
        err = svc_bt_hid_mouse(a->usage, 0, 0);
        if (err == ESP_OK) {
            lv_timer_t *t = lv_timer_create(hid_mouse_release_cb, HID_MOUSE_RELEASE_MS, NULL);
            if (t != NULL) lv_timer_set_repeat_count(t, 1);
        }
    } else {
        err = svc_bt_hid_mouse(0, a->dx, a->dy);
    }

    if (err == ESP_OK) {
        fw_ui_toast("已发送", 1500);
    } else {
        fw_ui_toast("发送失败", 1500);
    }
}

static void make_hid_cell(lv_obj_t *parent, size_t idx)
{
    const hid_action_t *a = &HID_ACTIONS[idx];

    lv_obj_t *cell = lv_button_create(parent);
    lv_obj_set_size(cell, CELL_W, CELL_H);
    lv_obj_set_style_bg_color(cell, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(cell, 8, 0);
    lv_obj_set_style_shadow_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_add_event_cb(cell, hid_action_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)idx);

    lv_obj_t *lb = lv_label_create(cell);
    lv_label_set_text(lb, a->name);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
}

/* ---------------------------------- 事件 ---------------------------------- */

static void on_state_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    refresh_state();
    lvgl_port_unlock();
}

static void on_scan_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    lvgl_port_lock(0);
    scan_finish();
    lvgl_port_unlock();
}

/* ---------------------------------- 页面搭建 ---------------------------------- */

static lv_obj_t *make_page(lv_obj_t *host)
{
    lv_obj_t *page = lv_obj_create(host);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, 8, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(page, false);
    return page;
}

static void page_show(int page);

static void hid_page_cb(lv_event_t *e)
{
    (void)e;
    page_show(PAGE_HID);
}

static void build_main_page(lv_obj_t *page)
{
    /* 头部：状态（撑满）+ 广播 / 扫描 / 键鼠 */
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_size(bar, lv_pct(100), HEADER_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_state_icon = fw_ui_icon(bar, &icon_ui_bt, fw_theme_color_text_disabled());

    s_state_lb = lv_label_create(bar);
    lv_obj_set_flex_grow(s_state_lb, 1);
    lv_label_set_text(s_state_lb, "");
    lv_obj_set_style_text_font(s_state_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_state_lb, fw_theme_color_text_primary(), 0);

    s_adv_btn = fw_ui_icon_btn(bar, &icon_ui_play, NULL, 36, adv_cb, NULL);
    fw_ui_icon_btn(bar, &icon_ui_search, NULL, 36, scan_cb, NULL);
    fw_ui_icon_btn(bar, &icon_ui_keyboard, NULL, 36, hid_page_cb, NULL);

    /* 周边设备：撑满剩余空间（需要滚动的是它自己，页面不滚） */
    s_list = fw_ui_list(page, NULL);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
}

static void build_hid_page(lv_obj_t *page)
{
    lv_obj_t *grid = lv_obj_create(page);
    lv_obj_set_size(grid, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, GRID_GAP_X, 0);
    lv_obj_set_style_pad_row(grid, GRID_GAP_Y, 0);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    for (size_t i = 0; i < HID_ACTION_COUNT; i++) {
        make_hid_cell(grid, i);
    }
}

static void page_show(int page)
{
    s_page_cur = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_page[i] != NULL) lv_obj_set_hidden(s_page[i], i != page);
    }
    refresh_state();
}

/* ---------------------------------- 生命周期 ---------------------------------- */

static void *bt_on_create(void)
{
    lvgl_port_lock(0);

    s_foreground = false;

    lv_obj_t *host = NULL;
    s_root = fw_ui_page(&host);

    for (int i = 0; i < PAGE_COUNT; i++) {
        s_page[i] = make_page(host);
    }
    build_main_page(s_page[PAGE_MAIN]);
    build_hid_page(s_page[PAGE_HID]);

    page_show(PAGE_MAIN);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void bt_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    svc_event_bus_unsubscribe(SVC_EVENT_BT_STATE_CHANGED, on_state_evt);
    svc_event_bus_unsubscribe(SVC_EVENT_BT_SCAN_DONE, on_scan_evt);
}

/* on_start 与 on_resume 都挂：从桌面进来是 on_start，从返回栈回来是 on_resume */
static void bt_on_enter(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    svc_event_bus_subscribe(SVC_EVENT_BT_STATE_CHANGED, on_state_evt, NULL);
    svc_event_bus_subscribe(SVC_EVENT_BT_SCAN_DONE, on_scan_evt, NULL);

    lvgl_port_lock(0);
    refresh_state();
    if (fill_scan_list() == 0 && !s_scanning) {
        scan_begin();        /* 和 Wi-Fi 一样：进来先自动扫一次 */
    }
    lvgl_port_unlock();
}

static void bt_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    if (s_scan_guard != NULL) {
        lv_timer_delete(s_scan_guard);
        s_scan_guard = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_state_lb = NULL;
    s_state_icon = NULL;
    s_adv_btn = NULL;
    s_list = NULL;
    for (int i = 0; i < PAGE_COUNT; i++) s_page[i] = NULL;
    lvgl_port_unlock();
}

/* 返回键：在键鼠页则回主页，主页交给框架退出 App */
static bool bt_on_back(void *ctx)
{
    (void)ctx;

    if (s_page_cur != PAGE_MAIN) {
        lvgl_port_lock(0);
        page_show(PAGE_MAIN);
        lvgl_port_unlock();
        return true;
    }
    return false;
}

const fw_app_desc_t app_bt_desc = {
    .name = "BT",
    .title = "蓝牙 BLE",
    .icon_64 = &icon_home_bt,
    .symbol = LV_SYMBOL_BLUETOOTH,
    .on_create = bt_on_create,
    .on_start = bt_on_enter,
    .on_pause = bt_on_pause,
    .on_resume = bt_on_enter,
    .on_destroy = bt_on_destroy,
    .on_back = bt_on_back,
};
