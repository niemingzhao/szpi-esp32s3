/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Bluetooth（蓝牙）
 *
 * 从机角色：显示协议栈状态、开关广播、扫描周边设备，并作为键鼠从机发送 HID 报告。
 * 中心角色：点扫描到的设备连接它，进 GATT 页浏览服务 / 特征，读特征值、订阅通知。
 *
 * 主页按角色分成两个标签（返回键交给 on_back）：
 *   从机·被连接：协议栈状态 + 广播开关 + 键鼠入口（配对完成后才可用）
 *   主机·主动连：中心链路状态（带对端名字）+ 扫描 + 设备列表（连上后自动进 GATT 页）
 * 子页：
 *   键鼠页：4 列 × 4 行网格，按功能分区（方向键 / 键盘常用 / 鼠标点击 / 鼠标移动）
 *   GATT 页：服务列表 ←→ 特征列表（点特征读值；头部按钮对选中特征读 / 订阅）
 *
 * 中心角色的回调在 Bluedroid 的 BTC 任务里，界面更新一律经 lv_async_call 交回 LVGL 任务。
 * 连接在离开本 App 后仍然保留（换主题会走 on_destroy + on_create，断开连接会把主题切换搞坏），
 * 要断开就再点一次那台设备。
 *
 * HID 报告是"已配对加密"之后才允许发送的（见 svc_bt_hid.h），所以主页的键鼠按钮
 * 和键鼠页里每个按键都用就绪判定：主页按钮未就绪时置灰，点了提示先去配对（设备名
 * 就是 SZPI-OS）；键鼠页里点击时也用 svc_bt_hid_is_ready() 判定，未就绪只给 Toast
 * 提示、不硬发。
 *
 * 键鼠页的按键支持长按连发（方向键 / 鼠标移动尤其需要），连发期间不再弹提示。
 */

#include "app_bt.h"
#include "fw_common.h"
#include "fw_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app.bt";

#define BT_SCAN_SECONDS     5
#define BT_SCAN_MAX         16
/* 扫描事件万一没回来时的兜底（正常 SCAN_DONE 会先到，定时器随即删掉） */
#define BT_SCAN_GUARD_MS    (BT_SCAN_SECONDS * 1000 + 3000)

#define HEADER_H            30
#define TAB_H               32
/* 4 列 × 4 行：70×4 + 4×3 = 292 ≤ 296，42×4 + 6×3 = 186 ≤ 188 */
#define CELL_W              70
#define CELL_H              42
#define GRID_GAP_X          4
#define GRID_GAP_Y          6

/* GATT 页：一次最多展示这么多服务 / 特征，读到的值截到这个长度 */
#define GATT_SVC_MAX        8
#define GATT_CHAR_MAX       8
#define GATT_VALUE_MAX      65

/* 鼠标移动步长（相对坐标，-127 ~ 127） */
#define HID_MOUSE_MOVE_STEP 20

enum { PAGE_MAIN = 0, PAGE_HID, PAGE_GATT, PAGE_COUNT };
enum { GATT_VIEW_SERVICES = 0, GATT_VIEW_CHARS };
/* 主页的两个标签：从机（被别人连）/ 主机（主动连别人），两块内容用显隐切换 */
enum { TAB_SLAVE = 0, TAB_MASTER, TAB_COUNT };

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
    { "Tab",        HID_ACT_KEY,         SVC_BT_HID_KEY_TAB,         0, 0 },
    { "退格",       HID_ACT_KEY,         SVC_BT_HID_KEY_BACKSPACE,   0, 0 },
    { "空格",       HID_ACT_KEY,         SVC_BT_HID_KEY_SPACE,       0, 0 },
    /* 回车 + 鼠标点击 */
    { "回车",       HID_ACT_KEY,         SVC_BT_HID_KEY_ENTER,       0, 0 },
    { "鼠标左键",   HID_ACT_MOUSE_CLICK, SVC_BT_HID_MOUSE_LEFT,      0, 0 },
    { "鼠标中键",   HID_ACT_MOUSE_CLICK, SVC_BT_HID_MOUSE_MIDDLE,    0, 0 },
    { "鼠标右键",   HID_ACT_MOUSE_CLICK, SVC_BT_HID_MOUSE_RIGHT,     0, 0 },
    /* 鼠标移动 */
    { "鼠标上移",   HID_ACT_MOUSE_MOVE,  0, 0, -HID_MOUSE_MOVE_STEP },
    { "鼠标下移",   HID_ACT_MOUSE_MOVE,  0, 0,  HID_MOUSE_MOVE_STEP },
    { "鼠标左移",   HID_ACT_MOUSE_MOVE,  0, -HID_MOUSE_MOVE_STEP, 0 },
    { "鼠标右移",   HID_ACT_MOUSE_MOVE,  0,  HID_MOUSE_MOVE_STEP, 0 },
};

#define HID_ACTION_COUNT (sizeof(HID_ACTIONS) / sizeof(HID_ACTIONS[0]))

/* 常见 16 位 UUID 的中文名（查不到就显示 UUID 本身） */
typedef struct {
    uint16_t uuid16;
    const char *name;
} uuid_name_t;

static const uuid_name_t SVC_NAMES[] = {
    { 0x1800, "通用访问" },
    { 0x1801, "通用属性" },
    { 0x180A, "设备信息" },
    { 0x180D, "心率" },
    { 0x180F, "电量" },
    { 0x1812, "人机接口" },
    { 0x181A, "环境感知" },
};

static const uuid_name_t CHAR_NAMES[] = {
    { 0x2A00, "设备名" },
    { 0x2A07, "发射功率" },
    { 0x2A19, "电量" },
    { 0x2A24, "型号" },
    { 0x2A25, "序列号" },
    { 0x2A26, "固件版本" },
    { 0x2A27, "硬件版本" },
    { 0x2A29, "厂商名" },
};

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page[PAGE_COUNT] = { 0 };
static int s_page_cur = PAGE_MAIN;

static lv_obj_t *s_tab_btn[TAB_COUNT] = { 0 };   /* 主页标签行 */
static lv_obj_t *s_tab_page[TAB_COUNT] = { 0 };  /* 主页两块内容 */
static uint8_t s_tab = TAB_MASTER;

/* 从机标签：状态 + 广播 / 键鼠 */
static lv_obj_t *s_slave_icon = NULL;
static lv_obj_t *s_slave_lb = NULL;
static lv_obj_t *s_adv_btn = NULL;
static lv_obj_t *s_hid_btn = NULL;

/* 主机标签：状态 + GATT / 扫描 + 设备列表 */
static lv_obj_t *s_master_icon = NULL;
static lv_obj_t *s_master_lb = NULL;
static lv_obj_t *s_gatt_btn = NULL;
static lv_obj_t *s_list = NULL;
static lv_timer_t *s_scan_guard = NULL;
static lv_timer_t *s_state_timer = NULL;         /* 1 s 状态轮询（HID 就绪、中心链路没有事件） */
static bool s_scanning = false;
static bool s_scanned = false;
static bool s_foreground = false;

/* GATT 页状态 */
static lv_obj_t *s_gatt_title = NULL;
static lv_obj_t *s_gatt_list = NULL;
static int s_gatt_view = GATT_VIEW_SERVICES;
static svc_bt_central_service_t s_sel_svc;
static svc_bt_central_char_t s_sel_char;
static bool s_sel_svc_valid = false;
static bool s_sel_char_valid = false;
static uint16_t s_last_handle = 0;                  /* 最近一次读到的特征句柄 */
static char s_last_value[GATT_VALUE_MAX] = "";      /* 最近一次读到的值（已转成可显示文本） */
static uint16_t s_sub_handle = 0;                   /* 已订阅的特征句柄 */
static bool s_discovering = false;

/* 换设备时先记下目标，等断开事件到了再连 */
static uint8_t s_pending_bda[6];
static uint8_t s_pending_type = 0;
static bool s_connect_pending = false;

/* 中心角色事件（BTC 任务里填，LVGL 任务里消费）。界面只按"最新一次状态"处理，
 * 所以用 s_evt_pending 合并：同一个事件被连排多次没有意义 */
static volatile svc_bt_central_evt_t s_evt_kind;
static volatile esp_err_t s_evt_err;
static volatile uint16_t s_evt_handle;
static volatile uint16_t s_evt_reason;
static char s_evt_value[GATT_VALUE_MAX];
static bool s_evt_pending = false;

/* ---------------------------------- 工具 ---------------------------------- */

static void uuid_text(const svc_bt_uuid_t *u, char *out, size_t len)
{
    if (u->is_128) {
        /* 128 位 UUID 只显示后 4 字节：蓝牙基础 UUID 里它就是有效信息 */
        snprintf(out, len, "%02X%02X%02X%02X",
                 u->uuid128[12], u->uuid128[13], u->uuid128[14], u->uuid128[15]);
    } else {
        snprintf(out, len, "0x%04X", u->uuid16);
    }
}

static const char *uuid_name(const svc_bt_uuid_t *u, const uuid_name_t *tab, size_t n)
{
    if (u->is_128) return NULL;

    for (size_t i = 0; i < n; i++) {
        if (tab[i].uuid16 == u->uuid16) return tab[i].name;
    }
    return NULL;
}

static bool uuid_eq(const svc_bt_uuid_t *a, const svc_bt_uuid_t *b)
{
    if (a->is_128 != b->is_128) return false;
    if (a->is_128) return memcmp(a->uuid128, b->uuid128, sizeof(a->uuid128)) == 0;
    return a->uuid16 == b->uuid16;
}

/* 特征值转成可显示文本：2 字节以内一律按 hex —— 短值多半是数值，1 字节的 0x55(=85%)
 * 要是走文本判定会显示成字母 U；3 字节以上再按"全是可打印字符就当文本"，否则 hex */
static void render_value(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    if (data == NULL || len == 0 || out_len == 0) {
        if (out_len > 0) out[0] = '\0';
        return;
    }

    bool text = (len > 2);
    for (size_t i = 0; text && i < len; i++) {
        if (data[i] < 0x20 || data[i] > 0x7E) {
            text = false;
        }
    }

    size_t n = 0;
    if (text) {
        n = (len < out_len - 1) ? len : out_len - 1;
        memcpy(out, data, n);
    } else {
        for (size_t i = 0; i < len && (n + 3) < out_len; i++) {
            n += (size_t)snprintf(out + n, out_len - n, "%02X ", data[i]);
        }
        if (n > 0) n--;                 /* 去掉末尾空格 */
    }
    out[n] = '\0';
}

/* ---------------------------------- 状态 ---------------------------------- */

/* HID 是否真的能发：从机链路已连接 + 已配对加密（两者都满足才允许发报告） */
static bool hid_ready(void)
{
    return svc_bt_is_connected() && svc_bt_hid_is_ready();
}

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

/* 对端设备名 / 页面切换（定义在 GATT 一节，这里状态轮询要用，先声明） */
static void peer_name(char *out, size_t len);
static void slave_peer_name(char *out, size_t len);
static size_t fill_scan_list(void);
static void page_show(int page);

/* 从机标签：协议栈状态 + 广播按钮图标 + 键鼠按钮就绪色 */
static void refresh_slave_state(void)
{
    const svc_bt_state_t st = svc_bt_get_state();
    const bool active = (st == SVC_BT_STATE_ADVERTISING || st == SVC_BT_STATE_CONNECTED);

    if (s_slave_lb != NULL) {
        if (st == SVC_BT_STATE_CONNECTED) {
            /* 连的是哪一台（HID 主机）：状态栏 / 主机页各有自己的连接，这里带名字免得混淆 */
            char who[SVC_BT_NAME_MAX];
            slave_peer_name(who, sizeof(who));

            char buf[SVC_BT_NAME_MAX + 16];
            snprintf(buf, sizeof(buf), "已连接 %s", who);
            lv_label_set_text(s_slave_lb, buf);
        } else {
            lv_label_set_text(s_slave_lb, state_text(st));
        }
    }
    if (s_slave_icon != NULL) {
        lv_obj_set_style_image_recolor(s_slave_icon,
                                       active ? fw_theme_color_accent()
                                              : fw_theme_color_text_disabled(), 0);
    }
    if (s_adv_btn != NULL) {
        lv_obj_t *icon = lv_obj_get_child(s_adv_btn, 0);
        if (icon != NULL) {
            /* 广播中用暂停、未广播用播放；已连着主机时这个按钮用来断开 */
            const void *src = &icon_ui_play;
            if (st == SVC_BT_STATE_ADVERTISING) {
                src = &icon_ui_pause;
            } else if (st == SVC_BT_STATE_CONNECTED) {
                src = &icon_ui_close;
            }
            lv_image_set_src(icon, src);
        }
    }
    /* 键鼠按钮：能发 HID 报告了才点亮 */
    if (s_hid_btn != NULL) {
        lv_obj_t *icon = lv_obj_get_child(s_hid_btn, 0);
        if (icon != NULL) {
            lv_obj_set_style_image_recolor(
                icon, hid_ready() ? fw_theme_color_accent()
                                  : fw_theme_color_text_primary(), 0);
        }
    }
}

/* 主机标签：中心链路状态（带上对端名字）+ GATT 按钮就绪色 */
static void refresh_master_state(void)
{
    svc_bt_central_status_t st;
    memset(&st, 0, sizeof(st));
    svc_bt_central_get_status(&st);

    if (s_master_icon != NULL) {
        lv_obj_set_style_image_recolor(s_master_icon,
                                       (st.connected || st.connecting)
                                           ? fw_theme_color_accent()
                                           : fw_theme_color_text_disabled(), 0);
    }
    if (s_master_lb != NULL) {
        if (st.connecting) {
            lv_label_set_text(s_master_lb, "连接中…");
        } else if (st.connected) {
            char who[SVC_BT_NAME_MAX];
            peer_name(who, sizeof(who));

            char buf[SVC_BT_NAME_MAX + 16];
            snprintf(buf, sizeof(buf), "已连接 %s", who);
            lv_label_set_text(s_master_lb, buf);
        } else {
            lv_label_set_text(s_master_lb, "未连接");
        }
    }
    /* GATT 按钮：中心连接建立后点亮 */
    if (s_gatt_btn != NULL) {
        lv_obj_t *icon = lv_obj_get_child(s_gatt_btn, 0);
        if (icon != NULL) {
            lv_obj_set_style_image_recolor(
                icon, st.connected ? fw_theme_color_accent()
                                   : fw_theme_color_text_primary(), 0);
        }
    }
}

/* 列表里的"当前连接项"标记：只有连接状态或对端变了才重填列表 ——
 * 重填会重置滚动位置，不能每秒都做 */
static uint8_t s_marked_bda[6];
static bool s_marked_on = false;

static void refresh_list_mark(void)
{
    svc_bt_central_status_t st;
    memset(&st, 0, sizeof(st));
    svc_bt_central_get_status(&st);

    const bool on = st.connected || st.connecting;
    if (on == s_marked_on &&
        (!on || memcmp(st.peer_bda, s_marked_bda, sizeof(s_marked_bda)) == 0)) {
        return;
    }

    memcpy(s_marked_bda, st.peer_bda, sizeof(s_marked_bda));
    s_marked_on = on;
    fill_scan_list();
}

/* 两个标签一起刷 */
static void refresh_state(void)
{
    refresh_slave_state();
    refresh_master_state();
    refresh_list_mark();
}

/* 1 s 轮询：从机的 HID 就绪（配对完成）和中心链路都没有事件能通知到界面，
 * 靠它保证连接 / 配对一完成按钮立刻变蓝（定时器只在进入前台时跑） */
static void state_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh_state();

    /* 中心链路的事件万一被丢（BTC 任务拿不到 LVGL 锁）：已经断开就自动退回主页，
     * 别停在 GATT 页看一份过期快照 */
    if (s_page_cur == PAGE_GATT) {
        svc_bt_central_status_t st;
        memset(&st, 0, sizeof(st));
        svc_bt_central_get_status(&st);
        if (!st.connected && !st.connecting) {
            page_show(PAGE_MAIN);
        }
    }
}

/* ------------------------------ GATT 页 ------------------------------ */

static void fill_gatt_list(void);

/* 行回调与列表填充互相调用（点服务要重填特征列表），所以先声明 */
static void svc_row_cb(lv_event_t *e);
static void char_row_cb(lv_event_t *e);

/* 对端设备的显示名：从扫描结果里按地址找，找不到就用地址（扫描结果可能已被覆盖） */
static void peer_name(char *out, size_t len)
{
    svc_bt_central_status_t st;
    svc_bt_central_get_status(&st);

    svc_bt_scan_result_t res[BT_SCAN_MAX];
    size_t n = 0;
    if (svc_bt_get_scan_results(res, BT_SCAN_MAX, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (res[i].name[0] != '\0' &&
                memcmp(res[i].bda, st.peer_bda, sizeof(st.peer_bda)) == 0) {
                snprintf(out, len, "%s", res[i].name);
                return;
            }
        }
    }

    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             st.peer_bda[0], st.peer_bda[1], st.peer_bda[2],
             st.peer_bda[3], st.peer_bda[4], st.peer_bda[5]);
}

/* 从机页显示的对端名：连我们的那台 HID 主机（同上，查不到名字就用地址） */
static void slave_peer_name(char *out, size_t len)
{
    uint8_t bda[6];
    if (!svc_bt_get_peer(bda)) {
        out[0] = '\0';
        return;
    }

    svc_bt_scan_result_t res[BT_SCAN_MAX];
    size_t n = 0;
    if (svc_bt_get_scan_results(res, BT_SCAN_MAX, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            if (res[i].name[0] != '\0' && memcmp(res[i].bda, bda, sizeof(bda)) == 0) {
                snprintf(out, len, "%s", res[i].name);
                return;
            }
        }
    }

    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/* GATT 页标题：「<设备名> · 服务 / 特征」，一眼看出在连谁 */
static void gatt_title_update(void)
{
    if (s_gatt_title == NULL) return;

    char who[SVC_BT_NAME_MAX];
    peer_name(who, sizeof(who));

    char buf[SVC_BT_NAME_MAX + 16];
    snprintf(buf, sizeof(buf), "%s · %s", who,
             (s_gatt_view == GATT_VIEW_CHARS) ? "特征" : "服务");
    lv_label_set_text(s_gatt_title, buf);
}

static void page_show_gatt(void)
{
    page_show(PAGE_GATT);
    gatt_title_update();
    fill_gatt_list();
}

static void fill_gatt_list(void)
{
    if (s_gatt_list == NULL) return;

    lv_obj_clean(s_gatt_list);

    if (!svc_bt_central_is_connected()) {
        fw_ui_list_hint(s_gatt_list, "未连接设备");
        return;
    }

    if (s_gatt_view == GATT_VIEW_SERVICES) {
        svc_bt_central_service_t svcs[GATT_SVC_MAX];
        size_t n = 0;

        if (svc_bt_central_get_services(svcs, GATT_SVC_MAX, &n) != ESP_OK || n == 0) {
            fw_ui_list_hint(s_gatt_list, s_discovering ? "正在发现服务…" : "没有发现服务");
            return;
        }

        for (size_t i = 0; i < n; i++) {
            char value[24];
            uuid_text(&svcs[i].uuid, value, sizeof(value));

            const char *name = uuid_name(&svcs[i].uuid, SVC_NAMES,
                                         sizeof(SVC_NAMES) / sizeof(SVC_NAMES[0]));
            lv_obj_t *item = fw_ui_list_add_value(s_gatt_list, name != NULL ? name : "服务",
                                                  value, svc_row_cb, (void *)(uintptr_t)i);
            if (s_sel_svc_valid && uuid_eq(&svcs[i].uuid, &s_sel_svc.uuid)) {
                fw_ui_list_mark(item, true);
            }
        }
        return;
    }

    /* 特征列表 */
    svc_bt_central_char_t chars[GATT_CHAR_MAX];
    size_t n = 0;

    if (!s_sel_svc_valid ||
        svc_bt_central_get_chars(&s_sel_svc, chars, GATT_CHAR_MAX, &n) != ESP_OK || n == 0) {
        fw_ui_list_hint(s_gatt_list, "没有特征");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        char value[GATT_VALUE_MAX];

        if (chars[i].handle == s_last_handle && s_last_value[0] != '\0') {
            snprintf(value, sizeof(value), "%s", s_last_value);
        } else if (chars[i].handle == s_sub_handle) {
            snprintf(value, sizeof(value), "已订阅");
        } else {
            uuid_text(&chars[i].uuid, value, sizeof(value));
        }

        const char *name = uuid_name(&chars[i].uuid, CHAR_NAMES,
                                     sizeof(CHAR_NAMES) / sizeof(CHAR_NAMES[0]));
        lv_obj_t *item = fw_ui_list_add_value(s_gatt_list, name != NULL ? name : "特征",
                                              value, char_row_cb, (void *)(uintptr_t)i);
        if (s_sel_char_valid && chars[i].handle == s_sel_char.handle) {
            fw_ui_list_mark(item, true);
        }
    }
}

static void svc_row_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);

    svc_bt_central_service_t svcs[GATT_SVC_MAX];
    size_t n = 0;
    if (svc_bt_central_get_services(svcs, GATT_SVC_MAX, &n) != ESP_OK || idx >= n) return;

    s_sel_svc = svcs[idx];
    s_sel_svc_valid = true;
    s_sel_char_valid = false;
    s_gatt_view = GATT_VIEW_CHARS;
    gatt_title_update();
    fill_gatt_list();
}

static void char_row_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);

    svc_bt_central_char_t chars[GATT_CHAR_MAX];
    size_t n = 0;
    if (!s_sel_svc_valid ||
        svc_bt_central_get_chars(&s_sel_svc, chars, GATT_CHAR_MAX, &n) != ESP_OK || idx >= n) {
        return;
    }

    s_sel_char = chars[idx];
    s_sel_char_valid = true;
    if (svc_bt_central_read(&s_sel_svc.uuid, &s_sel_char.uuid) != ESP_OK) {
        fw_ui_toast("读取失败", 1500);
    }
}

static void gatt_read_cb(lv_event_t *e)
{
    (void)e;

    if (!s_sel_char_valid) {
        fw_ui_toast("先点一个特征", 1500);
        return;
    }
    if (svc_bt_central_read(&s_sel_svc.uuid, &s_sel_char.uuid) != ESP_OK) {
        fw_ui_toast("读取失败", 1500);
    }
}

static void gatt_sub_cb(lv_event_t *e)
{
    (void)e;

    if (!s_sel_char_valid) {
        fw_ui_toast("先点一个特征", 1500);
        return;
    }

    const bool on = (s_sub_handle != s_sel_char.handle);
    if (on && (s_sel_char.properties & (SVC_BT_CHAR_PROP_NOTIFY | SVC_BT_CHAR_PROP_INDICATE)) == 0) {
        fw_ui_toast("该特征不支持订阅", 1800);
        return;
    }
    if (svc_bt_central_subscribe(&s_sel_svc.uuid, &s_sel_char.uuid, on) != ESP_OK) {
        fw_ui_toast(on ? "订阅失败" : "取消订阅失败", 1500);
    }
}

static void gatt_btn_cb(lv_event_t *e)
{
    (void)e;

    svc_bt_central_status_t st;
    svc_bt_central_get_status(&st);

    if (st.connecting) {
        fw_ui_toast("正在连接…", 1500);
        return;
    }
    if (!st.connected) {
        fw_ui_toast("先点设备连接", 1800);
        return;
    }

    /* 已经发现过就沿用，否则重新发现一次 */
    if (st.service_count == 0 && !s_discovering) {
        s_discovering = (svc_bt_central_discover() == ESP_OK);
    }
    page_show_gatt();
}

/* --------------------------- 中心角色事件 --------------------------- */

static void central_ui_cb(void *arg);

static void central_cb(const svc_bt_central_evt_data_t *evt, void *user)
{
    (void)user;

    /* 这里是 BTC 任务：不能久等，但也不能用 0 —— 0 会把事件直接丢掉，而丢一个
     * CONNECTED 就不会自动进 GATT 页、也不会自动发现服务。LVGL 一轮重绘可能拿着
     * 锁几十毫秒（320x240 @ 10 行缓冲一屏要刷 24 次），所以给 100 ms：
     * 拿不到才放弃（哪怕真放弃，状态还有 1 s 轮询兜底）。只拷状态 / 转文本，
     * 界面交给 LVGL 任务 */
    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "lvgl busy, central event dropped");
        return;
    }

    s_evt_kind = evt->evt;
    s_evt_err = evt->err;
    s_evt_handle = evt->handle;
    s_evt_reason = evt->reason;
    s_evt_value[0] = '\0';
    if ((evt->evt == SVC_BT_CENTRAL_EVT_READ || evt->evt == SVC_BT_CENTRAL_EVT_NOTIFY) &&
        evt->err == ESP_OK) {
        render_value(evt->data, evt->len, s_evt_value, sizeof(s_evt_value));
    }

    if (!s_evt_pending) {
        s_evt_pending = true;
        lv_async_call(central_ui_cb, NULL);
    }

    lvgl_port_unlock();
}

static void central_ui_cb(void *arg)
{
    (void)arg;

    /* 先把这次要处理的事件拷出来：清掉标志后，新事件可能立刻又填进来 */
    const svc_bt_central_evt_t kind = s_evt_kind;
    const esp_err_t err = s_evt_err;
    const uint16_t handle = s_evt_handle;
    const uint16_t reason = s_evt_reason;
    char value[GATT_VALUE_MAX];
    snprintf(value, sizeof(value), "%s", s_evt_value);
    s_evt_pending = false;

    switch (kind) {
    case SVC_BT_CENTRAL_EVT_CONNECTED:
        fw_ui_toast("已连接", 1500);
        s_sel_svc_valid = false;
        s_sel_char_valid = false;
        s_sub_handle = 0;
        s_last_handle = 0;
        s_last_value[0] = '\0';
        s_gatt_view = GATT_VIEW_SERVICES;
        s_discovering = (svc_bt_central_discover() == ESP_OK);
        page_show_gatt();
        break;

    case SVC_BT_CENTRAL_EVT_DISCONNECTED:
        if (err == ESP_ERR_TIMEOUT) {
            fw_ui_toast("连接超时", 1800);
        } else if (err != ESP_OK) {
            fw_ui_toast("连接失败", 1800);
        } else if (reason != 0) {
            /* 对端 / 协议栈给的断开原因码：排查"连上又被断开"就看它（串口日志里也有） */
            char msg[32];
            snprintf(msg, sizeof(msg), "连接已断开（0x%02X）", (unsigned)reason);
            fw_ui_toast(msg, 2500);
        } else {
            fw_ui_toast("连接已断开", 1500);
        }
        s_sel_svc_valid = false;
        s_sel_char_valid = false;
        s_sub_handle = 0;
        s_last_handle = 0;
        s_last_value[0] = '\0';
        s_discovering = false;
        if (s_page_cur == PAGE_GATT) page_show(PAGE_MAIN);

        if (s_connect_pending) {
            s_connect_pending = false;
            if (svc_bt_central_connect(s_pending_bda, s_pending_type) != ESP_OK) {
                fw_ui_toast("连接失败", 1800);
            }
        }
        break;

    case SVC_BT_CENTRAL_EVT_DISCOVER_DONE: {
        s_discovering = false;

        svc_bt_central_status_t st;
        svc_bt_central_get_status(&st);

        char msg[32];
        snprintf(msg, sizeof(msg), "发现 %u 个服务", (unsigned)st.service_count);
        fw_ui_toast(msg, 1800);

        if (s_page_cur == PAGE_GATT && s_gatt_view == GATT_VIEW_SERVICES) fill_gatt_list();
        break;
    }

    case SVC_BT_CENTRAL_EVT_READ:
        if (err == ESP_OK) {
            s_last_handle = handle;
            snprintf(s_last_value, sizeof(s_last_value), "%s", value);
        } else {
            fw_ui_toast("读取失败", 1500);
        }
        if (s_page_cur == PAGE_GATT && s_gatt_view == GATT_VIEW_CHARS) fill_gatt_list();
        break;

    case SVC_BT_CENTRAL_EVT_NOTIFY:
        if (err == ESP_OK) {
            s_last_handle = handle;
            snprintf(s_last_value, sizeof(s_last_value), "%s", value);
            if (s_page_cur == PAGE_GATT && s_gatt_view == GATT_VIEW_CHARS) fill_gatt_list();
        }
        break;

    case SVC_BT_CENTRAL_EVT_SUBSCRIBED:
        if (err == ESP_OK) {
            const bool was = (s_sub_handle == handle);
            s_sub_handle = was ? 0 : handle;
            fw_ui_toast(was ? "已取消订阅" : "已订阅", 1500);
        } else {
            fw_ui_toast("订阅失败", 1500);
        }
        if (s_page_cur == PAGE_GATT && s_gatt_view == GATT_VIEW_CHARS) fill_gatt_list();
        break;

    case SVC_BT_CENTRAL_EVT_AUTH_DONE: {
        /* 配对 / 重新加密的结果（失败时未加密的链路之外仍可读） */
        fw_ui_toast((err == ESP_OK) ? "配对完成" : "配对失败", 1800);

        /* 有些对端要求先配对才允许发现服务：那次发现会以"认证不足"失败，一个服务都没有。
         * 配完再发现一次，不用用户手动重进页面 */
        if (err == ESP_OK && !s_discovering) {
            svc_bt_central_status_t st;
            svc_bt_central_get_status(&st);
            if (st.connected && st.service_count == 0) {
                s_discovering = (svc_bt_central_discover() == ESP_OK);
            }
        }
        break;
    }

    default:
        break;
    }

    refresh_state();
}

/* ---------------------------------- 设备列表 ---------------------------------- */

static void device_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);

    svc_bt_scan_result_t res[BT_SCAN_MAX];
    size_t n = 0;
    if (svc_bt_get_scan_results(res, BT_SCAN_MAX, &n) != ESP_OK || idx >= n) return;

    svc_bt_central_status_t st;
    svc_bt_central_get_status(&st);

    if (st.connected) {
        if (memcmp(st.peer_bda, res[idx].bda, sizeof(st.peer_bda)) == 0) {
            svc_bt_central_disconnect();
            fw_ui_toast("断开中…", 1500);
        } else {
            /* 换设备：先记下目标，断开事件到了再连 */
            memcpy(s_pending_bda, res[idx].bda, sizeof(s_pending_bda));
            s_pending_type = res[idx].addr_type;
            s_connect_pending = true;
            fw_ui_toast("切换设备…", 1500);
            svc_bt_central_disconnect();
        }
        return;
    }

    /* 上一次连接还没结果：点同一台 = 取消，点别的先等（服务侧同一时刻只允许一次尝试） */
    if (st.connecting) {
        if (memcmp(st.peer_bda, res[idx].bda, sizeof(st.peer_bda)) == 0) {
            svc_bt_central_disconnect();
            fw_ui_toast("已取消连接", 1500);
        } else {
            fw_ui_toast("正在连接，请稍候", 1800);
        }
        return;
    }

    if (svc_bt_central_connect(res[idx].bda, res[idx].addr_type) == ESP_OK) {
        fw_ui_toast("连接中…", 1800);
    } else {
        fw_ui_toast("连接失败", 1800);
    }
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

    svc_bt_central_status_t st;
    svc_bt_central_get_status(&st);

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
        const bool is_peer = (memcmp(st.peer_bda, res[i].bda, sizeof(st.peer_bda)) == 0);
        if (st.connected && is_peer) {
            snprintf(rssi, sizeof(rssi), "已连接");
        } else if (st.connecting && is_peer) {
            snprintf(rssi, sizeof(rssi), "连接中");
        } else {
            snprintf(rssi, sizeof(rssi), "%d dBm", (int)res[i].rssi);
        }

        lv_obj_t *item = fw_ui_list_add_icon(s_list, &icon_ui_bt, who, rssi,
                                             device_cb, (void *)(uintptr_t)i);

        /* 和 Wi-Fi 列表同一套信号分色规则；当前（或正在）连接的那台再加主色描边 */
        if ((st.connected || st.connecting) && is_peer) {
            fw_ui_list_value_color(item, fw_theme_color_accent());
            fw_ui_list_mark(item, true);
        } else if (res[i].rssi >= -60) {
            fw_ui_list_value_color(item, fw_theme_color_success());
        } else if (res[i].rssi < -78) {
            fw_ui_list_value_color(item, fw_theme_color_warning());
        }
    }
    return count;
}

/* ---------------------------------- 广播 / 扫描 ---------------------------------- */

/* 从机页右上角按钮：未广播 → 开始广播；广播中 → 暂停广播；已连接 → 断开对端 */
static void slave_btn_cb(lv_event_t *e)
{
    (void)e;

    const svc_bt_state_t st = svc_bt_get_state();
    if (st == SVC_BT_STATE_ADVERTISING) {
        svc_bt_adv_stop();
    } else if (st == SVC_BT_STATE_READY) {
        svc_bt_adv_start();
    } else if (st == SVC_BT_STATE_CONNECTED) {
        /* 已连着主机：这个按钮用来断开（连接期间广播本来就是停的） */
        if (svc_bt_disconnect_peer() == ESP_OK) {
            fw_ui_toast("断开中…", 1500);
        } else {
            fw_ui_toast("断开失败", 1500);
        }
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
    if (hid_ready()) return true;

    if (!svc_bt_is_connected()) {
        fw_ui_toast("未连接，请先配对", 2000);
    } else {
        fw_ui_toast("配对未完成，请稍后重试", 2000);
    }
    return false;
}

static void hid_action_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= HID_ACTION_COUNT) return;
    if (!hid_guard()) return;

    const hid_action_t *a = &HID_ACTIONS[idx];

    esp_err_t err;
    if (a->kind == HID_ACT_KEY) {
        err = svc_bt_hid_key_click(a->usage, 0);
    } else if (a->kind == HID_ACT_MOUSE_CLICK) {
        err = svc_bt_hid_mouse_click(a->usage);      /* 按下 / 松开由服务侧负责 */
    } else {
        err = svc_bt_hid_mouse(0, a->dx, a->dy);
    }

    if (err != ESP_OK) {
        fw_ui_toast("发送失败", 1500);
        return;
    }
    /* 长按连发不提示（太吵）；鼠标移动本来就是连点，也不提示 */
    if (code == LV_EVENT_SHORT_CLICKED && a->kind != HID_ACT_MOUSE_MOVE) {
        fw_ui_toast("已发送", 800);
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
    /* 长按连发：方向键、鼠标移动靠它才挪得动 */
    lv_obj_add_event_cb(cell, hid_action_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(uintptr_t)idx);

    lv_obj_t *lb = lv_label_create(cell);
    lv_label_set_text(lb, a->name);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
}

/* 订阅回调跑在事件总线任务里（栈很小）：只置位，真正的界面刷新交给 LVGL 任务 ——
 * 填设备列表要创建十几行控件，直接在总线任务里跑会把它的栈压爆（实测过） */
static bool s_refresh_evt = false;
static bool s_scan_done_evt = false;
static bool s_ui_evt_queued = false;   /* 已经排了一次刷新，等 LVGL 任务跑完再排 */

static void ui_evt_cb(void *arg)
{
    (void)arg;

    s_ui_evt_queued = false;

    if (s_refresh_evt) {
        s_refresh_evt = false;
        refresh_state();
    }
    if (s_scan_done_evt) {
        s_scan_done_evt = false;
        scan_finish();
    }
}

static void schedule_ui_evt(void)
{
    /* 状态会在很短时间内连发（->1 ->2 ->3）：已经排了一次就等它跑完，别排一串 */
    if (s_ui_evt_queued) return;

    if (!lvgl_port_lock(100)) {
        /* 拿不到锁（极少见）：这次刷新丢掉，扫描那条还有兜底定时器收尾 */
        ESP_LOGW(TAG, "lvgl busy, ui event dropped");
        s_refresh_evt = false;
        s_scan_done_evt = false;
        return;
    }

    if (lv_async_call(ui_evt_cb, NULL) == LV_RESULT_OK) {
        s_ui_evt_queued = true;
    }
    lvgl_port_unlock();
}

static void on_state_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    s_refresh_evt = true;
    schedule_ui_evt();
}

static void on_scan_evt(const svc_event_t *evt, void *user)
{
    (void)evt;
    (void)user;

    s_scan_done_evt = true;
    schedule_ui_evt();
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

static lv_obj_t *make_header(lv_obj_t *page)
{
    lv_obj_t *bar = lv_obj_create(page);
    lv_obj_set_size(bar, lv_pct(100), HEADER_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return bar;
}

static void hid_page_cb(lv_event_t *e)
{
    (void)e;

    /* 没就绪就不让进：键鼠页里每个按键都发不了，进去只会一直弹提示 */
    if (!hid_ready()) {
        fw_ui_toast("先做设备配对", 2200);
        return;
    }
    page_show(PAGE_HID);
}

/* 标签行选中样式：和列表当前项同一套（主色描边 + 主色字） */
static void tab_refresh(void)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        if (s_tab_btn[i] == NULL) continue;

        const bool on = (i == (int)s_tab);
        lv_obj_set_style_border_color(s_tab_btn[i],
                                      on ? fw_theme_color_accent() : fw_theme_color_border(), 0);

        lv_obj_t *lb = lv_obj_get_child(s_tab_btn[i], 0);
        if (lb != NULL) {
            lv_obj_set_style_text_color(lb, on ? fw_theme_color_accent()
                                               : fw_theme_color_text_primary(), 0);
        }
    }
}

static void tab_show(uint8_t tab)
{
    s_tab = (tab < TAB_COUNT) ? tab : TAB_MASTER;

    for (int i = 0; i < TAB_COUNT; i++) {
        if (s_tab_page[i] != NULL) lv_obj_set_hidden(s_tab_page[i], i != (int)s_tab);
    }
    tab_refresh();
    refresh_state();
}

static void tab_cb(lv_event_t *e)
{
    const uint8_t tab = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (tab == s_tab) return;

    tab_show(tab);
}

static void build_main_page(lv_obj_t *page)
{
    /* 标签行：两个角色各一块内容，状态与设备列表不再混在一起 */
    lv_obj_t *bar = make_header(page);
    lv_obj_set_height(bar, TAB_H);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(bar);
        lv_obj_set_height(btn, TAB_H - 4);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, tab_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, (i == TAB_SLAVE) ? "从机·被连接" : "主机·主动连");
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_obj_center(lb);

        s_tab_btn[i] = btn;
    }

    /* 从机页：状态行（广播开关 + 键鼠入口）+ 使用提示 */
    s_tab_page[TAB_SLAVE] = make_page(page);
    lv_obj_set_flex_grow(s_tab_page[TAB_SLAVE], 1);

    lv_obj_t *row = make_header(s_tab_page[TAB_SLAVE]);
    s_slave_icon = fw_ui_icon(row, &icon_ui_bt, fw_theme_color_text_disabled());
    s_slave_lb = lv_label_create(row);
    lv_obj_set_flex_grow(s_slave_lb, 1);
    lv_label_set_text(s_slave_lb, "");
    lv_label_set_long_mode(s_slave_lb, LV_LABEL_LONG_MODE_DOTS);   /* 设备名过长不要撑破头部 */
    lv_obj_set_style_text_font(s_slave_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_slave_lb, fw_theme_color_text_primary(), 0);
    s_adv_btn = fw_ui_icon_btn(row, &icon_ui_play, NULL, 36, slave_btn_cb, NULL);
    s_hid_btn = fw_ui_icon_btn(row, &icon_ui_keyboard, NULL, 36, hid_page_cb, NULL);

    char hint_text[96];
    snprintf(hint_text, sizeof(hint_text),
             "手机或电脑在蓝牙列表里连「%s」，配对完成后「键鼠」可用", svc_identity_bt_name());

    lv_obj_t *hint = lv_label_create(s_tab_page[TAB_SLAVE]);
    lv_label_set_text(hint, hint_text);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);

    /* 主机页：状态行（GATT + 扫描）+ 设备列表撑满剩余空间 */
    s_tab_page[TAB_MASTER] = make_page(page);
    lv_obj_set_flex_grow(s_tab_page[TAB_MASTER], 1);

    row = make_header(s_tab_page[TAB_MASTER]);
    s_master_icon = fw_ui_icon(row, &icon_ui_bt, fw_theme_color_text_disabled());
    s_master_lb = lv_label_create(row);
    lv_obj_set_flex_grow(s_master_lb, 1);
    lv_label_set_text(s_master_lb, "");
    lv_label_set_long_mode(s_master_lb, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_master_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_master_lb, fw_theme_color_text_primary(), 0);
    fw_ui_icon_btn(row, &icon_ui_search, NULL, 36, scan_cb, NULL);
    s_gatt_btn = fw_ui_icon_btn(row, &icon_ui_link, NULL, 36, gatt_btn_cb, NULL);

    s_list = fw_ui_list(s_tab_page[TAB_MASTER], NULL);
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

static void build_gatt_page(lv_obj_t *page)
{
    /* 头部：标题（撑满）+ 读 / 订阅 */
    lv_obj_t *bar = make_header(page);

    s_gatt_title = lv_label_create(bar);
    lv_obj_set_flex_grow(s_gatt_title, 1);
    lv_label_set_long_mode(s_gatt_title, LV_LABEL_LONG_MODE_DOTS);   /* 设备名过长不要撑破头部 */
    lv_label_set_text(s_gatt_title, "服务");
    lv_obj_set_style_text_font(s_gatt_title, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_gatt_title, fw_theme_color_text_primary(), 0);

    fw_ui_icon_btn(bar, &icon_ui_refresh, NULL, 36, gatt_read_cb, NULL);
    fw_ui_icon_btn(bar, &icon_ui_loop, NULL, 36, gatt_sub_cb, NULL);

    s_gatt_list = fw_ui_list(page, NULL);
    lv_obj_set_width(s_gatt_list, lv_pct(100));
    lv_obj_set_flex_grow(s_gatt_list, 1);
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
    build_gatt_page(s_page[PAGE_GATT]);

    svc_bt_central_register_cb(central_cb, NULL);

    /* 界面重建（换主题 / 重进 App）：连接是服务层保持的，但 App 侧这些"进行中"的标志
     * 必须清掉 —— 重建期间丢了 DISCOVER_DONE / SCAN_DONE 的话，它们会一直挂着，
     * GATT 页停在"正在发现服务…"、扫描按钮也点不动 */
    s_gatt_view = GATT_VIEW_SERVICES;
    s_sel_svc_valid = false;
    s_sel_char_valid = false;
    s_discovering = false;
    s_scanning = false;
    s_scanned = false;
    page_show(PAGE_MAIN);
    tab_show(s_tab);              /* 套上标签的显隐与选中样式 */

    /* 状态轮询：先暂停，进入前台（on_start / on_resume）再跑 */
    s_state_timer = lv_timer_create(state_timer_cb, 1000, NULL);
    if (s_state_timer != NULL) {
        lv_timer_pause(s_state_timer);
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void bt_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;

    /* 后台收不到 SCAN_DONE，把扫描态清掉：否则回到前台会一直显示"正在扫描…"，
     * 而且 scan_begin() 会被它挡住、再也扫不了（兜底定时器醒来也会因 !s_scanning 空转） */
    s_scanning = false;

    if (s_state_timer != NULL) lv_timer_pause(s_state_timer);

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
    if (s_state_timer != NULL) lv_timer_resume(s_state_timer);
    refresh_state();
    if (fill_scan_list() == 0 && !s_scanning) {
        scan_begin();        /* 和 Wi-Fi 一样：进来先自动扫一次 */
    }
    lvgl_port_unlock();
}

static void bt_on_destroy(void *ctx)
{
    (void)ctx;

    /* 连接不在这里断：换主题也会走 on_destroy + on_create，断开连接会把主题切换搞坏 */
    svc_bt_central_unregister_cb(central_cb, NULL);

    /* 退订也要在这里做一遍：重建后台 App 时不一定先走 on_pause，不退订就会一直
     * 收到事件并往已销毁的界面上刷（on_pause 里退订是同一对，重复调用是安全的） */
    svc_event_bus_unsubscribe(SVC_EVENT_BT_STATE_CHANGED, on_state_evt);
    svc_event_bus_unsubscribe(SVC_EVENT_BT_SCAN_DONE, on_scan_evt);

    lvgl_port_lock(0);
    if (s_scan_guard != NULL) {
        lv_timer_delete(s_scan_guard);
        s_scan_guard = NULL;
    }
    if (s_state_timer != NULL) {
        lv_timer_delete(s_state_timer);
        s_state_timer = NULL;
    }
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_slave_icon = NULL;
    s_slave_lb = NULL;
    s_adv_btn = NULL;
    s_hid_btn = NULL;
    s_master_icon = NULL;
    s_master_lb = NULL;
    s_gatt_btn = NULL;
    s_list = NULL;
    s_gatt_title = NULL;
    s_gatt_list = NULL;
    for (int i = 0; i < TAB_COUNT; i++) {
        s_tab_btn[i] = NULL;
        s_tab_page[i] = NULL;
    }
    for (int i = 0; i < PAGE_COUNT; i++) s_page[i] = NULL;
    lvgl_port_unlock();
}

/* 返回键：GATT 页的特征视图 → 服务视图 → 主页；键鼠页 → 主页；主页交给框架退出 App */
static bool bt_on_back(void *ctx)
{
    (void)ctx;

    if (s_page_cur == PAGE_GATT) {
        lvgl_port_lock(0);
        if (s_gatt_view == GATT_VIEW_CHARS) {
            s_gatt_view = GATT_VIEW_SERVICES;
            gatt_title_update();
            fill_gatt_list();
        } else {
            page_show(PAGE_MAIN);
        }
        lvgl_port_unlock();
        return true;
    }

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
    .icon = &icon_home_bt,
    .on_create = bt_on_create,
    .on_start = bt_on_enter,
    .on_pause = bt_on_pause,
    .on_resume = bt_on_enter,
    .on_destroy = bt_on_destroy,
    .on_back = bt_on_back,
};
