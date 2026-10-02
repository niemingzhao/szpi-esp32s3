/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Weather（APP-WEATHER 天气）
 *
 * 数据来自 Open-Meteo（免 key、全球城市、支持中文）：
 *   - 城市搜索：geocoding-api.open-meteo.com/v1/search（按拼音 / 英文查，返回中文名）
 *   - 实况 + 今日：api.open-meteo.com/v1/forecast
 * 选定的城市（短名 + 经纬度）存在 svc_settings 的 "weather" namespace。
 *
 * 单屏布局（180 / 188）：
 *   头部（30）：城市按钮（点开搜索浮层）+ 刷新按钮
 *   实况卡（60）：第一行「温度 + 状态」（flex 行保证两者垂直对齐），第二行小字放今日高低温 / 日出日落
 *   详情格（3 列 × 3 行，22 px 行，每格有描边、标题左 / 数值右两端对齐）
 *
 * 搜索浮层：输入框（右侧「重置」）+ 常用城市快捷钮（24 个：中国主要城市 + 东亚 +
 * 世界主要城市，按拼音 / 英文查找）+ 键盘（点输入框才弹出、回车收起、点空白处收起）。
 * 结果列表列在**输入框下方**，输入框保留在页面上，方便改完再提交。
 * LVGL 键盘没有中文 IME，只能输拼音 / 英文；常用城市直接按拼音搜（省得手打），
 * 这是绕开"打不出中文"的主要入口。
 *
 * 主界面没有数据时也把类目全部渲染出来（数值用 --，实况卡写"无数据"），
 * 状态 / 错误文案放在实况卡右上。
 *
 * 异步请求注意点（svc_http_get_async 会新建临时任务，回调跑在 svc.http 任务里）：
 *   - 回调里碰 LVGL 必须加锁；请求类型通过 user 传进去，别用共享变量（并发会错位）
 *   - 同时只发一个请求（s_busy）：共用响应缓冲，也省一个 6 KB 栈的任务
 *   - 响应缓冲与结果数组常驻、不随 App 释放：请求在飞的时候可能被换主题销毁重建，
 *     释放会把内存交给还在写它的任务（use-after-free）
 */

#include "app_weather.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app.weather";

#define GEO_URL_FMT \
    "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=8&language=zh&format=json"

#define WX_URL_FMT \
    "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s" \
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation," \
    "weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,precipitation_sum" \
    "&timezone=auto&forecast_days=1"

#define SET_NS          "weather"
#define SET_KEY_CITY    "city"
#define SET_KEY_LAT     "lat"
#define SET_KEY_LON     "lon"

#define WX_BUF_SIZE     (8 * 1024)
#define WX_URL_MAX      512          /* 天气 URL 渲染后 367 字节；svc_net 的异步上限也是 512 */
#define WX_MIN_GAP_MS   60000        /* 进前台 1 分钟内不重复拉 */
#define GEO_MAX         8

#define HEADER_H        30
#define CARD_H          60
#define CELL_W          96
#define CELL_H          22
#define CELL_GAP        4
#define OVERLAY_TOP     28           /* 让开状态栏 */
#define KEYBOARD_H      120

/* 详情格（8 个，3 列 × 3 行，第 9 格空着） */
enum { D_FEELS = 0, D_HUMID, D_WIND, D_CLOUD, D_PRESSURE, D_PRECIP, D_GUST, D_UPDATED, D_COUNT };
static const char *D_LABEL[D_COUNT] = { "体感", "湿度", "风", "云量", "气压", "降水", "阵风", "更新" };

/* 常用城市：显示中文名，查询用拼音 / 英文（键盘没有中文 IME）。
 * 4 行 × 6 个：中国主要城市 + 东亚 + 世界主要城市。 */
typedef struct {
    const char *name;
    const char *query;
} chip_t;

static const chip_t CHIPS[] = {
    { "北京",   "beijing" },
    { "上海",   "shanghai" },
    { "广州",   "guangzhou" },
    { "深圳",   "shenzhen" },
    { "杭州",   "hangzhou" },
    { "成都",   "chengdu" },
    { "武汉",   "wuhan" },
    { "西安",   "xian" },
    { "香港",   "hong kong" },
    { "台北",   "taipei" },
    { "东京",   "tokyo" },
    { "首尔",   "seoul" },
    { "新加坡", "singapore" },
    { "迪拜",   "dubai" },
    { "伦敦",   "london" },
    { "巴黎",   "paris" },
    { "纽约",   "new york" },
    { "悉尼",   "sydney" },
    { "洛杉矶", "los angeles" },
    { "莫斯科", "moscow" },
    { "柏林",   "berlin" },
    { "罗马",   "rome" },
    { "温哥华", "vancouver" },
    { "开罗",   "cairo" },
};
#define CHIP_COUNT  (sizeof(CHIPS) / sizeof(CHIPS[0]))
#define CHIP_W      47                                  /* 6×47 + 5×2 = 292，能放下 3 个汉字 */
#define CHIP_H      28
#define CHIP_ROW_H  (4 * CHIP_H + 3 * 4)
#define CHIP_TOP    64                                  /* 输入框下面一点，键盘弹出时整块收起 */

/* 请求类型：通过 svc_http 的 user 传给回调，避免并发时错位 */
typedef enum { REQ_NONE = 0, REQ_GEO, REQ_GEO_AUTO, REQ_WX } req_t;

/* 搜索结果（常驻分配，理由见文件头） */
typedef struct {
    char label[80];
    char name[32];
    char lat[16];
    char lon[16];
} geo_item_t;

static geo_item_t *s_items = NULL;
static char *s_buf = NULL;
static size_t s_item_count = 0;
static bool s_busy = false;
static int64_t s_last_fetch_ms = 0;
static bool s_foreground = false;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_city_lb = NULL;
static lv_obj_t *s_temp_lb = NULL;
static lv_obj_t *s_cond_lb = NULL;
static lv_obj_t *s_sub_lb = NULL;
static lv_obj_t *s_cell[D_COUNT] = { 0 };

/* 搜索浮层 */
static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_ta = NULL;
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_chips = NULL;
static lv_obj_t *s_results = NULL;

static char s_city[32] = { 0 };
static char s_lat[16] = { 0 };
static char s_lon[16] = { 0 };

/* 前置声明 */
static void on_http(const char *body, esp_err_t err, void *user);
static void apply_city(size_t idx);
static void refresh_weather(bool force);
static void search_city(const char *keyword, bool auto_pick);
static void overlay_close(void);
static void geo_pick_cb(lv_event_t *e);

/* ------------------------------- 小工具 ------------------------------- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *wmo_text(int code)
{
    switch (code) {
    case 0:  return "晴";
    case 1:  return "晴间多云";
    case 2:  return "多云";
    case 3:  return "阴";
    case 45: case 48: return "雾";
    case 51: return "小毛毛雨";
    case 53: return "毛毛雨";
    case 55: return "大毛毛雨";
    case 56: case 57: return "冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: return "小阵雨";
    case 81: return "阵雨";
    case 82: return "强阵雨";
    case 85: return "小阵雪";
    case 86: return "阵雪";
    case 95: return "雷阵雨";
    case 96: return "雷阵雨伴冰雹";
    case 99: return "强雷暴伴冰雹";
    default: return "未知";
    }
}

static const char *wind_dir_text(double deg)
{
    static const char *DIRS[] = { "北", "东北", "东", "东南", "南", "西南", "西", "西北" };

    int d = (int)(deg + 0.5) % 360;
    if (d < 0) d += 360;
    return DIRS[((d + 22) / 45) % 8];
}

/* 城市名可能是中文，查询串要 percent-encode */
static void url_encode(const char *src, char *dst, size_t len)
{
    static const char HEX[] = "0123456789ABCDEF";

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0' && o + 4 < len; p++) {
        const bool plain = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
                           (*p >= 'a' && *p <= 'z') || *p == '-' || *p == '_' ||
                           *p == '.' || *p == '~';
        if (plain) {
            dst[o++] = (char)*p;
        } else {
            dst[o++] = '%';
            dst[o++] = HEX[*p >> 4];
            dst[o++] = HEX[*p & 0x0F];
        }
    }
    dst[o] = '\0';
}

/* cJSON 对缺失字段返回 NAN，直接格式化会打出 "nan"，统一走这几个取值口 */
static double jnum(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (v == NULL || !cJSON_IsNumber(v)) return def;
    return cJSON_GetNumberValue(v);
}

/* 取 daily 数组里第一项的数字 */
static double jitem(const cJSON *obj, const char *key, double def)
{
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj, key), 0);
    if (v == NULL || !cJSON_IsNumber(v)) return def;
    return cJSON_GetNumberValue(v);
}

/* 取 daily 数组里第一项的字符串 */
static const char *jstr_at(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_GetStringValue(cJSON_GetArrayItem(v, 0));
}

/* ------------------------------- 界面写入 ------------------------------- */

static void set_cell(int idx, const char *value)
{
    if (idx < 0 || idx >= D_COUNT || s_cell[idx] == NULL || value == NULL) return;
    lv_label_set_text(s_cell[idx], value);
}

/* 没有数据时的占位：类目全部渲染出来、数值用 --；卡片左侧显示"无数据"，
 * 用和右侧提示一样大的字体，保证两者在同一条基线上 */
static void fill_placeholder(void)
{
    if (s_temp_lb != NULL) {
        lv_obj_set_style_text_font(s_temp_lb, fw_asset_font_cn_large(), 0);
        lv_label_set_text(s_temp_lb, "无数据");
    }
    if (s_cond_lb != NULL) lv_label_set_text(s_cond_lb, "");
    if (s_sub_lb != NULL) lv_label_set_text(s_sub_lb, "");

    for (int i = 0; i < D_COUNT; i++) {
        set_cell(i, "--");
    }
}

/* 状态 / 错误提示：文案放实况卡右上（原来天气状况的位置），详情格保持 -- 占位 */
static void show_status(const char *text)
{
    fill_placeholder();
    if (s_cond_lb != NULL && text != NULL) {
        lv_label_set_text(s_cond_lb, text);
    }
}

/* ------------------------------- 天气解析 ------------------------------- */

static void parse_weather(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        show_status("解析失败");
        return;
    }

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *day = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (cur == NULL) {
        show_status("数据异常");
        cJSON_Delete(root);
        return;
    }

    char buf[80];

    if (s_temp_lb != NULL) {
        snprintf(buf, sizeof(buf), "%.1f°C", jnum(cur, "temperature_2m", 0));
        lv_obj_set_style_text_font(s_temp_lb, fw_asset_font_24(), 0);   /* 有数据用大字 */
        lv_label_set_text(s_temp_lb, buf);
    }
    if (s_cond_lb != NULL) {
        lv_label_set_text(s_cond_lb, wmo_text((int)jnum(cur, "weather_code", -1)));
    }

    snprintf(buf, sizeof(buf), "%.1f°C", jnum(cur, "apparent_temperature", 0));
    set_cell(D_FEELS, buf);

    snprintf(buf, sizeof(buf), "%d%%", (int)(jnum(cur, "relative_humidity_2m", 0) + 0.5));
    set_cell(D_HUMID, buf);

    snprintf(buf, sizeof(buf), "%s%d", wind_dir_text(jnum(cur, "wind_direction_10m", 0)),
             (int)(jnum(cur, "wind_speed_10m", 0) + 0.5));
    set_cell(D_WIND, buf);

    snprintf(buf, sizeof(buf), "%d%%", (int)(jnum(cur, "cloud_cover", 0) + 0.5));
    set_cell(D_CLOUD, buf);

    snprintf(buf, sizeof(buf), "%d", (int)(jnum(cur, "pressure_msl", 0) + 0.5));
    set_cell(D_PRESSURE, buf);

    snprintf(buf, sizeof(buf), "%.1fmm", jnum(cur, "precipitation", 0));
    set_cell(D_PRECIP, buf);

    snprintf(buf, sizeof(buf), "%.1f", jnum(cur, "wind_gusts_10m", 0));
    set_cell(D_GUST, buf);

    const char *utc = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cur, "time"));
    if (utc != NULL) {
        const char *t = strchr(utc, 'T');
        set_cell(D_UPDATED, t ? t + 1 : utc);
    }

    /* 卡片第二行：今日高低温 · 日出日落（API 用 timezone=auto，时间就是当地时刻） */
    if (day != NULL && s_sub_lb != NULL) {
        char hi[24] = { 0 };
        char sun[40] = { 0 };

        const cJSON *tmax = cJSON_GetObjectItemCaseSensitive(day, "temperature_2m_max");
        const cJSON *tmin = cJSON_GetObjectItemCaseSensitive(day, "temperature_2m_min");
        if (tmax != NULL && tmin != NULL) {
            snprintf(hi, sizeof(hi), "今日 %.0f/%.0f°C",
                     jitem(day, "temperature_2m_max", 0),
                     jitem(day, "temperature_2m_min", 0));
        }

        const char *rise = jstr_at(day, "sunrise");
        const char *set = jstr_at(day, "sunset");
        if (rise != NULL && set != NULL) {
            const char *a = strchr(rise, 'T');
            const char *b = strchr(set, 'T');
            snprintf(sun, sizeof(sun), "日出 %s 日落 %s", a ? a + 1 : rise, b ? b + 1 : set);
        }

        if (hi[0] != '\0' && sun[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s · %s", hi, sun);
        } else if (hi[0] != '\0') {
            snprintf(buf, sizeof(buf), "%s", hi);
        } else {
            snprintf(buf, sizeof(buf), "%s", sun);
        }
        lv_label_set_text(s_sub_lb, buf);
    }

    s_last_fetch_ms = now_ms();
    cJSON_Delete(root);
}

static void parse_geo(const char *json)
{
    s_item_count = 0;

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return;

    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *it = NULL;

    cJSON_ArrayForEach(it, results) {
        if (s_item_count >= GEO_MAX) break;

        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        const char *admin1 = cJSON_GetStringValue(cJSON_GetObjectItem(it, "admin1"));
        const char *country = cJSON_GetStringValue(cJSON_GetObjectItem(it, "country"));
        const cJSON *lat = cJSON_GetObjectItem(it, "latitude");
        const cJSON *lon = cJSON_GetObjectItem(it, "longitude");
        if (name == NULL || lat == NULL || lon == NULL) continue;

        geo_item_t *g = &s_items[s_item_count];
        snprintf(g->name, sizeof(g->name), "%s", name);
        snprintf(g->lat, sizeof(g->lat), "%.4f", cJSON_GetNumberValue(lat));
        snprintf(g->lon, sizeof(g->lon), "%.4f", cJSON_GetNumberValue(lon));

        if (admin1 != NULL && country != NULL) {
            snprintf(g->label, sizeof(g->label), "%s · %s · %s", name, admin1, country);
        } else if (country != NULL) {
            snprintf(g->label, sizeof(g->label), "%s · %s", name, country);
        } else {
            snprintf(g->label, sizeof(g->label), "%s", name);
        }
        s_item_count++;
    }

    cJSON_Delete(root);
}

/* ------------------------------- 搜索浮层 ------------------------------- */

static void overlay_close(void)
{
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_ta = NULL;
        s_kb = NULL;
        s_chips = NULL;
        s_results = NULL;
    }
}

/* 显示结果区（会收起键盘与常用城市）；msg 非空时只显示一行提示 */
static void results_begin(const char *msg)
{
    if (s_results == NULL) return;

    if (s_kb != NULL) lv_obj_set_hidden(s_kb, true);
    if (s_chips != NULL) lv_obj_set_hidden(s_chips, true);

    lv_obj_set_hidden(s_results, false);
    lv_obj_clean(s_results);
    if (msg != NULL) fw_ui_list_add(s_results, msg, NULL, NULL);
}

static void fill_geo_list(void)
{
    if (s_results == NULL) return;

    results_begin(NULL);
    if (s_item_count == 0) {
        fw_ui_list_add(s_results, "没有找到城市", NULL, NULL);
        return;
    }
    for (size_t i = 0; i < s_item_count; i++) {
        fw_ui_list_add(s_results, s_items[i].label, geo_pick_cb, (void *)(uintptr_t)i);
    }
}

static void geo_pick_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    apply_city(idx);
}

static void chip_cb(lv_event_t *e)
{
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= CHIP_COUNT) return;

    fw_ui_toast("搜索中…", 1500);
    search_city(CHIPS[idx].query, true);
}

static void ta_tap_cb(lv_event_t *e)
{
    (void)e;

    /* 用 CLICKED 而不是 FOCUSED：浮层关掉再开、或键盘收起后，输入框可能已经是
     * 焦点状态，FOCUSED 不会再发，键盘就弹不出来 */
    if (s_kb != NULL) {
        lv_keyboard_set_textarea(s_kb, s_ta);   /* 重新绑定（取消时解绑过） */
        lv_obj_set_hidden(s_kb, false);
        lv_obj_move_foreground(s_kb);
    }
    if (s_chips != NULL) lv_obj_set_hidden(s_chips, true);
}

static void kb_hide(void)
{
    if (s_kb != NULL) {
        lv_obj_set_hidden(s_kb, true);
        lv_keyboard_set_textarea(s_kb, NULL);   /* 解绑，免得下次打开时输入框已带焦点 */
    }
    /* 没在显示搜索结果时，把常用城市放回来 */
    if (s_chips != NULL && (s_results == NULL || lv_obj_is_hidden(s_results))) {
        lv_obj_set_hidden(s_chips, false);
    }
}

static void kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        char kw[64] = { 0 };
        if (s_ta != NULL) snprintf(kw, sizeof(kw), "%s", lv_textarea_get_text(s_ta));
        kb_hide();
        if (kw[0] != '\0') {
            results_begin("搜索中…");
            search_city(kw, false);
        }
    } else if (code == LV_EVENT_CANCEL) {
        kb_hide();
    }
}

/* 收起键盘 + 输入框失焦（点空白处 / 点重置都走这里） */
static void keyboard_dismiss(void)
{
    if (s_kb != NULL && !lv_obj_is_hidden(s_kb)) {
        kb_hide();
    }
    if (s_ta != NULL) {
        lv_obj_clear_state(s_ta, LV_STATE_FOCUSED);
    }
}

/* 点浮层空白处（不是输入框 / 城市按钮 / 结果项）：输入框失焦、收起键盘 */
static void overlay_click_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    if (target != s_overlay && target != s_chips) return;

    keyboard_dismiss();
}

/* 重置：清空输入、收起键盘、隐藏结果，回到刚进浮层时的常用城市列表 */
static void overlay_reset(void)
{
    if (s_ta != NULL) lv_textarea_set_text(s_ta, "");
    if (s_kb != NULL) {
        lv_obj_set_hidden(s_kb, true);
        lv_keyboard_set_textarea(s_kb, NULL);
    }
    if (s_results != NULL) {
        lv_obj_set_hidden(s_results, true);
        lv_obj_clean(s_results);
    }
    if (s_chips != NULL) lv_obj_set_hidden(s_chips, false);

    keyboard_dismiss();
    s_item_count = 0;
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    overlay_reset();
}

/* 头部按钮：symbol 可空、text 可空；w > 0 固定宽度，否则撑满剩余空间 */
static lv_obj_t *make_header_btn(lv_obj_t *parent, const char *symbol, const char *text,
                                 lv_coord_t w, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, HEADER_H - 2);
    if (w > 0) {
        lv_obj_set_width(btn, w);
    } else {
        lv_obj_set_flex_grow(btn, 1);
    }
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 4);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, NULL);

    if (symbol != NULL) {
        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(icon, fw_theme_color_accent(), 0);
        if (text != NULL) {
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
        } else {
            lv_obj_center(icon);
        }
    }

    if (text != NULL) {
        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, text);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lb, lv_pct(72));
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 32, 0);
    }
    return btn;
}

static void overlay_open(void)
{
    if (s_overlay != NULL) return;
    if (s_root == NULL) return;

    s_overlay = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_overlay, 8, 0);
    lv_obj_set_style_pad_top(s_overlay, OVERLAY_TOP, 0);
    lv_obj_set_scrollable(s_overlay, false);

    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = lv_label_create(s_overlay);
    lv_label_set_text(hint, "城市名（拼音 / 英文）");
    lv_obj_set_style_text_font(hint, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(hint, fw_theme_color_text_secondary(), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 输入行：输入框（撑满）+ 重置（清空输入、收起键盘、回到刚进来的样子） */
    lv_obj_t *in_row = lv_obj_create(s_overlay);
    lv_obj_set_size(in_row, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(in_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(in_row, 0, 0);
    lv_obj_set_style_pad_all(in_row, 0, 0);
    lv_obj_set_style_pad_column(in_row, 6, 0);
    lv_obj_set_scrollable(in_row, false);
    lv_obj_set_flex_flow(in_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(in_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(in_row, LV_ALIGN_TOP_MID, 0, 18);

    s_ta = lv_textarea_create(in_row);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_placeholder_text(s_ta, "shanghai / Tokyo");
    lv_obj_set_height(s_ta, 36);
    lv_obj_set_flex_grow(s_ta, 1);
    lv_obj_set_style_text_font(s_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_ta, fw_theme_color_text_primary(), 0);
    lv_obj_add_event_cb(s_ta, ta_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *reset = lv_button_create(in_row);
    lv_obj_set_size(reset, 46, 32);
    lv_obj_set_style_bg_color(reset, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(reset, 1, 0);
    lv_obj_set_style_border_color(reset, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(reset, 6, 0);
    lv_obj_set_style_shadow_width(reset, 0, 0);
    lv_obj_set_style_pad_all(reset, 0, 0);
    lv_obj_add_event_cb(reset, reset_cb, LV_EVENT_SHORT_CLICKED, NULL);

    lv_obj_t *reset_lb = lv_label_create(reset);
    lv_label_set_text(reset_lb, "重置");
    lv_obj_set_style_text_font(reset_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(reset_lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(reset_lb);

    /* 常用城市：3 行 × 6 个（47×6 + 2×5 = 292 ≤ 296），键盘弹出时会整体收起 */
    s_chips = lv_obj_create(s_overlay);
    lv_obj_set_size(s_chips, lv_pct(100), CHIP_ROW_H);
    lv_obj_set_style_bg_opa(s_chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chips, 0, 0);
    lv_obj_set_style_pad_all(s_chips, 0, 0);
    lv_obj_set_style_pad_column(s_chips, 2, 0);
    lv_obj_set_style_pad_row(s_chips, 4, 0);
    lv_obj_set_scrollable(s_chips, false);
    lv_obj_set_flex_flow(s_chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_align(s_chips, LV_ALIGN_TOP_MID, 0, CHIP_TOP);

    for (size_t i = 0; i < CHIP_COUNT; i++) {
        lv_obj_t *chip = lv_button_create(s_chips);
        lv_obj_set_size(chip, CHIP_W, CHIP_H);
        lv_obj_set_style_bg_color(chip, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, fw_theme_color_border(), 0);
        lv_obj_set_style_radius(chip, 6, 0);
        lv_obj_set_style_shadow_width(chip, 0, 0);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_add_event_cb(chip, chip_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *lb = lv_label_create(chip);
        lv_label_set_text(lb, CHIPS[i].name);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_obj_center(lb);
    }

    /* 键盘：先藏起来，点输入框再弹出（不然会压住常用城市） */
    s_kb = lv_keyboard_create(s_overlay);
    lv_obj_set_size(s_kb, lv_pct(100), KEYBOARD_H);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 8);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_set_hidden(s_kb, true);

    /* 结果列表：放在输入框下方（保留输入框，便于改完再提交），不用盖住整页。
     * 88 起的可用高度 = 232 − 88 = 144，留 4 px 余量 */
    s_results = fw_ui_list(s_overlay, NULL);
    lv_obj_set_size(s_results, lv_pct(100), 140);
    lv_obj_align(s_results, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_hidden(s_results, true);
}

/* ------------------------------- 请求 ------------------------------- */

static void apply_city(size_t idx)
{
    if (idx >= s_item_count) return;

    const geo_item_t *it = &s_items[idx];

    snprintf(s_city, sizeof(s_city), "%s", it->name);
    snprintf(s_lat, sizeof(s_lat), "%s", it->lat);
    snprintf(s_lon, sizeof(s_lon), "%s", it->lon);

    svc_settings_set_str(SET_NS, SET_KEY_CITY, s_city);
    svc_settings_set_str(SET_NS, SET_KEY_LAT, s_lat);
    svc_settings_set_str(SET_NS, SET_KEY_LON, s_lon);

    if (s_city_lb != NULL) lv_label_set_text(s_city_lb, s_city);

    overlay_close();
    refresh_weather(true);
}

static void search_city(const char *keyword, bool auto_pick)
{
    if (keyword == NULL || keyword[0] == '\0') return;
    if (s_buf == NULL || s_items == NULL) {
        fw_ui_toast("内存不足", 2000);
        return;
    }
    if (s_busy) {
        fw_ui_toast("请稍候", 1500);
        return;
    }

    char enc[96];
    url_encode(keyword, enc, sizeof(enc));
    s_item_count = 0;

    char url[WX_URL_MAX];
    snprintf(url, sizeof(url), GEO_URL_FMT, enc);
    if (svc_http_get_async(url, s_buf, WX_BUF_SIZE, on_http,
                           (void *)(intptr_t)(auto_pick ? REQ_GEO_AUTO : REQ_GEO)) != ESP_OK) {
        results_begin("搜索失败");
        return;
    }
    s_busy = true;
}

static void refresh_weather(bool force)
{
    if (s_lat[0] == '\0' || s_lon[0] == '\0') {
        show_status("请选择城市");
        if (force) fw_ui_toast("请先选择城市", 2000);
        return;
    }
    if (s_buf == NULL) {
        show_status("内存不足");
        return;
    }
    if (s_busy) {
        if (force) fw_ui_toast("请稍候", 1500);
        return;
    }
    if (!force && s_last_fetch_ms != 0 && (now_ms() - s_last_fetch_ms) < WX_MIN_GAP_MS) return;

    svc_net_status_t st;
    if (svc_net_get_status(&st) == ESP_OK && !st.wifi_connected) {
        show_status("未联网");
        if (force) fw_ui_toast("请先连接 Wi-Fi", 2000);
        return;
    }

    if (s_last_fetch_ms == 0) show_status("加载中…");   /* 首次才显示加载态，刷新时保留旧数据 */

    char url[WX_URL_MAX];
    snprintf(url, sizeof(url), WX_URL_FMT, s_lat, s_lon);
    if (svc_http_get_async(url, s_buf, WX_BUF_SIZE, on_http, (void *)(intptr_t)REQ_WX) != ESP_OK) {
        show_status("请求失败");
        return;
    }
    s_busy = true;
}

/* 回调跑在 svc.http 任务里，碰 LVGL 必须加锁 */
static void on_http(const char *body, esp_err_t err, void *user)
{
    const req_t req = (req_t)(intptr_t)user;

    lvgl_port_lock(0);

    /* 本请求已结束就放行，后面（自动选城）可能还要立刻发一个天气请求 */
    s_busy = false;

    if (err != ESP_OK) {
        if (req == REQ_WX) {
            show_status("请求失败");
        } else {
            results_begin("搜索失败");
        }
    } else if (req == REQ_WX) {
        parse_weather(body);
    } else {
        parse_geo(body);
        if (req == REQ_GEO_AUTO && s_item_count > 0) {
            apply_city(0);              /* 常用城市：直接取第一个结果（`apply_city` 会关浮层并拉天气） */
        } else {
            fill_geo_list();
        }
    }

    lvgl_port_unlock();
}

/* ------------------------------- 事件 ------------------------------- */

static void city_cb(lv_event_t *e)
{
    (void)e;
    overlay_open();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    refresh_weather(true);
}

/* ------------------------------- 生命周期 ------------------------------- */

static void *weather_on_create(void)
{
    lvgl_port_lock(0);

    s_foreground = false;

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    /* 常驻缓冲：请求在飞的时候 App 可能被换主题销毁重建，这些内存不随 App 释放 */
    if (s_buf == NULL) s_buf = malloc(WX_BUF_SIZE);
    if (s_items == NULL) s_items = malloc(sizeof(geo_item_t) * GEO_MAX);
    if (s_buf == NULL || s_items == NULL) ESP_LOGE(TAG, "no memory for http buffers");

    /* 头部：城市按钮（撑满）+ 刷新按钮 */
    lv_obj_t *bar = lv_obj_create(body);
    lv_obj_set_size(bar, lv_pct(100), HEADER_H);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 读上次选的城市（城市名放在按钮里的第 2 个子对象） */
    svc_settings_get_str(SET_NS, SET_KEY_CITY, s_city, sizeof(s_city), "");
    svc_settings_get_str(SET_NS, SET_KEY_LAT, s_lat, sizeof(s_lat), "");
    svc_settings_get_str(SET_NS, SET_KEY_LON, s_lon, sizeof(s_lon), "");

    lv_obj_t *city_btn = make_header_btn(bar, LV_SYMBOL_GPS,
                                         s_city[0] != '\0' ? s_city : "请选择城市",
                                         -1, city_cb);
    s_city_lb = lv_obj_get_child(city_btn, 1);
    make_header_btn(bar, LV_SYMBOL_REFRESH, NULL, 36, refresh_cb);

    /* 实况卡：第一行「温度 + 状态」（flex 行，保证两者垂直对齐），第二行小字 */
    lv_obj_t *card = lv_obj_create(body);
    lv_obj_set_size(card, lv_pct(100), CARD_H);
    lv_obj_set_style_bg_color(card, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *line1 = lv_obj_create(card);
    lv_obj_set_size(line1, lv_pct(100), 26);
    lv_obj_set_style_bg_opa(line1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line1, 0, 0);
    lv_obj_set_style_pad_all(line1, 0, 0);
    lv_obj_set_scrollable(line1, false);
    lv_obj_set_flex_flow(line1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(line1, LV_ALIGN_TOP_LEFT, 0, 0);

    s_temp_lb = lv_label_create(line1);
    lv_label_set_text(s_temp_lb, "无数据");
    lv_obj_set_style_text_font(s_temp_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_temp_lb, fw_theme_color_text_primary(), 0);

    s_cond_lb = lv_label_create(line1);
    lv_label_set_text(s_cond_lb, "");
    lv_obj_set_style_text_font(s_cond_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_cond_lb, fw_theme_color_accent(), 0);

    s_sub_lb = lv_label_create(card);
    lv_label_set_text(s_sub_lb, "");
    lv_obj_set_style_text_font(s_sub_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_sub_lb, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_sub_lb, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* 详情格：3 列 × 3 行（96×3 + 4×2 = 296 宽正好铺满，第 9 格空着） */
    lv_obj_t *grid = lv_obj_create(body);
    lv_obj_set_size(grid, lv_pct(100), CELL_H * 3 + CELL_GAP * 2);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, CELL_GAP, 0);
    lv_obj_set_style_pad_row(grid, CELL_GAP, 0);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    /* 必须 START（不能 SPACE_BETWEEN）：9 宫格只有 8 项时，SpaceBetween 会把最后一格
     * 推到最右边，看着像"跳到第三列"；START 让不满的行左对齐、优先充满格位 */
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    /* 每格：卡片底 + 1 px 描边（表格感），格内标题靠左、数值靠右两端对齐 */
    for (int i = 0; i < D_COUNT; i++) {
        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_style_bg_color(cell, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, fw_theme_color_border(), 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_style_pad_left(cell, 5, 0);
        lv_obj_set_style_pad_right(cell, 5, 0);
        lv_obj_set_scrollable(cell, false);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, D_LABEL[i]);
        lv_obj_set_style_text_font(name, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(name, fw_theme_color_text_secondary(), 0);

        lv_obj_t *val = lv_label_create(cell);
        lv_label_set_text(val, "--");
        lv_obj_set_style_text_font(val, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(val, fw_theme_color_text_primary(), 0);
        s_cell[i] = val;
    }

    if (s_city[0] == '\0') {
        lv_label_set_text(s_city_lb, "请选择城市");
        fill_placeholder();
    } else {
        show_status("加载中…");
        refresh_weather(true);       /* 每次创建都拉一次（进前台那次走 on_resume 的节流版本） */
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (city=%s)", s_city[0] != '\0' ? s_city : "-");
    return s_root;
}

static void weather_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
}

/* on_start 与 on_resume 都挂：从桌面进来是 on_start，从返回栈回来是 on_resume */
static void weather_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;

    lvgl_port_lock(0);
    refresh_weather(false);          /* 一分钟内不重复拉；手动刷新走 refresh_cb */
    lvgl_port_unlock();
}

static void weather_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_overlay = NULL;                /* 浮层挂在根屏上，随它一起删掉 */
    s_ta = NULL;
    s_kb = NULL;
    s_chips = NULL;
    s_results = NULL;

    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_city_lb = NULL;
    s_temp_lb = NULL;
    s_cond_lb = NULL;
    s_sub_lb = NULL;
    for (int i = 0; i < D_COUNT; i++) s_cell[i] = NULL;
    lvgl_port_unlock();

    /* s_buf / s_items 故意不释放：见文件头（请求可能还在写） */
}

/* 返回键：浮层开着就先关浮层 */
static bool weather_on_back(void *ctx)
{
    (void)ctx;

    if (s_overlay == NULL) return false;

    lvgl_port_lock(0);
    overlay_close();
    lvgl_port_unlock();
    return true;
}

const fw_app_desc_t app_weather_desc = {
    .name = "Weather",
    .title = "天气",
    .icon_64 = &icon_home_weather,
    .symbol = LV_SYMBOL_TINT,
    .on_create = weather_on_create,
    .on_start = weather_on_resume,
    .on_pause = weather_on_pause,
    .on_resume = weather_on_resume,
    .on_destroy = weather_on_destroy,
    .on_back = weather_on_back,
};
