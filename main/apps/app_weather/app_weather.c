/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Weather（APP-WEATHER 天气）
 *
 * 数据来自 Open-Meteo（免 key、全球城市、支持中文，走 https）：
 *   - 城市搜索：geocoding-api.open-meteo.com/v1/search
 *   - 实况 + 今日：api.open-meteo.com/v1/forecast
 * 选定的城市（名称 + 经纬度）存在 svc_settings 的 "weather" namespace 里。
 *
 * svc_http_get_async 的回调在 svc.http 任务里执行，回调里碰 LVGL 必须加锁。
 */

#include "app_weather.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
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
#define WX_TIMEOUT_MS   15000
/* 请求 URL 的缓冲：WX_URL_FMT 的固定部分约 353 字节，再加纬度/经度各最多 15 字符，
 * 320 装不下（会被静默截断成半截参数），512 留足余量。 */
#define URL_MAX         512
#define GEO_MAX         8
#define KB_HEIGHT       130

/* 主界面上的数值行 */
enum {
    ROW_FEELS = 0,
    ROW_HUMID,
    ROW_WIND,
    ROW_CLOUD,
    ROW_PRESSURE,
    ROW_PRECIP,
    ROW_GUST,
    ROW_TODAY,
    ROW_SUN,
    ROW_UPDATED,
    ROW_COUNT,
};

static const char *ROW_TEXT[ROW_COUNT] = {
    "体感", "湿度", "风", "云量", "气压", "降水", "阵风", "今日", "日出日落", "更新",
};

static const char *ROW_SYMBOL[ROW_COUNT] = {
    LV_SYMBOL_TINT, LV_SYMBOL_TINT, LV_SYMBOL_REFRESH, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_DOWNLOAD,
    LV_SYMBOL_DOWNLOAD, LV_SYMBOL_REFRESH, LV_SYMBOL_UP, LV_SYMBOL_UP, LV_SYMBOL_LIST,
};

/* 搜索结果（堆上分配，随搜索页存活） */
typedef struct {
    char label[80];
    char name[32];
    char lat[16];
    char lon[16];
} geo_item_t;

static geo_item_t *s_items = NULL;
static size_t s_item_count = 0;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_city_row = NULL;
static lv_obj_t *s_temp_lb = NULL;
static lv_obj_t *s_cond_lb = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_rows[ROW_COUNT] = { 0 };

/* 城市输入浮层 */
static lv_obj_t *s_input = NULL;
static lv_obj_t *s_input_ta = NULL;

static char *s_buf = NULL;
static char s_city[80] = { 0 };
static char s_lat[16] = { 0 };
static char s_lon[16] = { 0 };

typedef enum { PENDING_NONE, PENDING_GEO, PENDING_WX } pending_t;
static pending_t s_pending = PENDING_NONE;

/* 两个请求共用同一个回调，用 s_pending 区分 */
static void on_http(const char *body, esp_err_t err, void *user);

/* ------------------------------- 小工具 ------------------------------- */

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

static void set_row(int idx, const char *value)
{
    if (idx >= 0 && idx < ROW_COUNT && s_rows[idx] != NULL) {
        fw_ui_row_btn_value(s_rows[idx], value);
    }
}

static void set_all_rows(const char *text)
{
    for (int i = 0; i < ROW_COUNT; i++) {
        set_row(i, text);
    }
}

/* ------------------------------- 搜索结果 ------------------------------- */

static void geo_pick_cb(lv_event_t *e)
{
    const uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_item_count) return;

    const geo_item_t *it = &s_items[idx - 1];

    snprintf(s_city, sizeof(s_city), "%s", it->label);
    snprintf(s_lat, sizeof(s_lat), "%s", it->lat);
    snprintf(s_lon, sizeof(s_lon), "%s", it->lon);

    svc_settings_set_str(SET_NS, SET_KEY_CITY, s_city);
    svc_settings_set_str(SET_NS, SET_KEY_LAT, s_lat);
    svc_settings_set_str(SET_NS, SET_KEY_LON, s_lon);

    if (s_city_row != NULL) fw_ui_row_btn_value(s_city_row, s_city);
    if (s_list != NULL) lv_obj_clean(s_list);

    set_all_rows("…");
    s_pending = PENDING_WX;

    char url[URL_MAX];
    snprintf(url, sizeof(url), WX_URL_FMT, s_lat, s_lon);
    if (svc_http_get_async(url, s_buf, WX_BUF_SIZE, on_http, NULL) != ESP_OK) {
        s_pending = PENDING_NONE;
        set_all_rows("失败");
    }
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

static void fill_geo_list(void)
{
    if (s_list == NULL) return;

    lv_obj_clean(s_list);

    if (s_item_count == 0) {
        fw_ui_list_add(s_list, "没有找到城市", NULL, NULL);
        return;
    }
    for (size_t i = 0; i < s_item_count; i++) {
        fw_ui_list_add(s_list, s_items[i].label, geo_pick_cb, (void *)(uintptr_t)(i + 1));
    }
}

/* ------------------------------- 天气解析 ------------------------------- */

static void parse_weather(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        set_all_rows("解析失败");
        return;
    }

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *day = cJSON_GetObjectItemCaseSensitive(root, "daily");

    char buf[64];

    if (cur != NULL) {
        const int code = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "weather_code"));
        const double temp = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "temperature_2m"));

        if (s_cond_lb != NULL) lv_label_set_text(s_cond_lb, wmo_text(code));
        if (s_temp_lb != NULL) {
            snprintf(buf, sizeof(buf), "%.1f°C", temp);
            lv_label_set_text(s_temp_lb, buf);
        }

        snprintf(buf, sizeof(buf), "%.1f°C",
                 cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "apparent_temperature")));
        set_row(ROW_FEELS, buf);

        snprintf(buf, sizeof(buf), "%d%%",
                 (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "relative_humidity_2m")));
        set_row(ROW_HUMID, buf);

        const double wdir = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_direction_10m"));
        const double wspd = cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_speed_10m"));
        snprintf(buf, sizeof(buf), "%s %.1f km/h", wind_dir_text(wdir), wspd);
        set_row(ROW_WIND, buf);

        snprintf(buf, sizeof(buf), "%d%%",
                 (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "cloud_cover")));
        set_row(ROW_CLOUD, buf);

        snprintf(buf, sizeof(buf), "%.1f hPa",
                 cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "pressure_msl")));
        set_row(ROW_PRESSURE, buf);

        snprintf(buf, sizeof(buf), "%.1f mm",
                 cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "precipitation")));
        set_row(ROW_PRECIP, buf);

        snprintf(buf, sizeof(buf), "%.1f km/h",
                 cJSON_GetNumberValue(cJSON_GetObjectItem(cur, "wind_gusts_10m")));
        set_row(ROW_GUST, buf);
    }

    if (day != NULL) {
        const cJSON *tmax = cJSON_GetObjectItem(day, "temperature_2m_max");
        const cJSON *tmin = cJSON_GetObjectItem(day, "temperature_2m_min");
        const cJSON *t0 = cJSON_GetArrayItem(tmax, 0);
        const cJSON *t1 = cJSON_GetArrayItem(tmin, 0);
        if (t0 != NULL && t1 != NULL) {
            snprintf(buf, sizeof(buf), "%.1f / %.1f°C",
                     cJSON_GetNumberValue(t0), cJSON_GetNumberValue(t1));
            set_row(ROW_TODAY, buf);
        }

        const char *sunrise = cJSON_GetStringValue(cJSON_GetArrayItem(
            cJSON_GetObjectItem(day, "sunrise"), 0));
        const char *sunset = cJSON_GetStringValue(cJSON_GetArrayItem(
            cJSON_GetObjectItem(day, "sunset"), 0));
        if (sunrise != NULL && sunset != NULL) {
            /* "2026-10-01T05:47" 只取时间部分 */
            const char *s1 = strchr(sunrise, 'T');
            const char *s2 = strchr(sunset, 'T');
            snprintf(buf, sizeof(buf), "%s / %s", s1 ? s1 + 1 : sunrise, s2 ? s2 + 1 : sunset);
            set_row(ROW_SUN, buf);
        }
    }

    const char *now = cJSON_GetStringValue(cJSON_GetObjectItem(cur, "time"));
    if (now != NULL) {
        const char *t = strchr(now, 'T');
        set_row(ROW_UPDATED, t ? t + 1 : now);
    }

    cJSON_Delete(root);
}

static void refresh_weather(void)
{
    if (s_lat[0] == '\0' || s_lon[0] == '\0') {
        set_all_rows("请先选城市");
        return;
    }

    set_all_rows("…");
    s_pending = PENDING_WX;

    char url[URL_MAX];
    snprintf(url, sizeof(url), WX_URL_FMT, s_lat, s_lon);
    if (svc_http_get_async(url, s_buf, WX_BUF_SIZE, on_http, NULL) != ESP_OK) {
        s_pending = PENDING_NONE;
        set_all_rows("请求失败");
    }
}

static void search_city(const char *keyword)
{
    char enc[96];
    url_encode(keyword, enc, sizeof(enc));

    s_item_count = 0;
    s_pending = PENDING_GEO;

    char url[URL_MAX];
    snprintf(url, sizeof(url), GEO_URL_FMT, enc);
    if (svc_http_get_async(url, s_buf, WX_BUF_SIZE, on_http, NULL) != ESP_OK) {
        s_pending = PENDING_NONE;
        fw_ui_toast("搜索失败", 2000);
    }
}

/* ------------------------------- 输入浮层 ------------------------------- */

static void input_close(void)
{
    if (s_input != NULL) {
        lv_obj_delete(s_input);
        s_input = NULL;
        s_input_ta = NULL;
    }
}

static void kb_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        char kw[64] = { 0 };
        if (s_input_ta != NULL) {
            snprintf(kw, sizeof(kw), "%s", lv_textarea_get_text(s_input_ta));
        }
        input_close();

        if (kw[0] != '\0') {
            if (s_list != NULL) {
                lv_obj_clean(s_list);
                fw_ui_list_add(s_list, "搜索中…", NULL, NULL);
            }
            search_city(kw);
        }
    } else if (code == LV_EVENT_CANCEL) {
        input_close();
    }
}

static void input_open(void)
{
    if (s_input != NULL) return;

    s_input = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input);
    lv_obj_set_size(s_input, lv_pct(100), lv_pct(100));
    lv_obj_center(s_input);
    lv_obj_set_style_bg_color(s_input, fw_theme_color_bg_primary(), 0);
    lv_obj_set_style_bg_opa(s_input, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_input, 8, 0);
    lv_obj_set_scrollable(s_input, false);

    lv_obj_t *lb = lv_label_create(s_input);
    lv_label_set_text(lb, "搜索城市");
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 0, 0);

    s_input_ta = lv_textarea_create(s_input);
    lv_textarea_set_one_line(s_input_ta, true);
    lv_textarea_set_placeholder_text(s_input_ta, "城市名，如 上海 / Tokyo");
    lv_obj_set_size(s_input_ta, lv_pct(100), 36);
    lv_obj_align(s_input_ta, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_text_font(s_input_ta, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_input_ta, fw_theme_color_text_primary(), 0);

    lv_obj_t *kb = lv_keyboard_create(s_input);
    lv_obj_set_size(kb, lv_pct(100), KB_HEIGHT);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_input_ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);
}

/* ------------------------------- 请求回调 ------------------------------- */

static void on_http(const char *body, esp_err_t err, void *user)
{
    (void)user;

    const pending_t which = s_pending;
    s_pending = PENDING_NONE;

    lvgl_port_lock(0);

    if (err != ESP_OK) {
        if (which == PENDING_GEO) {
            if (s_list != NULL) {
                lv_obj_clean(s_list);
                fw_ui_list_add(s_list, "搜索失败", NULL, NULL);
            }
        } else if (which == PENDING_WX) {
            set_all_rows("请求失败");
        }
    } else if (which == PENDING_GEO) {
        parse_geo(body);
        fill_geo_list();
    } else if (which == PENDING_WX) {
        parse_weather(body);
    }

    lvgl_port_unlock();
}

/* ------------------------------- 事件 ------------------------------- */

static void city_cb(lv_event_t *e)
{
    (void)e;
    input_open();
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    refresh_weather();
}

static void *weather_on_create(void)
{
    lvgl_port_lock(0);

    lv_obj_t *body = NULL;
    s_root = fw_ui_page(&body);

    s_city_row = fw_ui_row_btn(body, LV_SYMBOL_EDIT, "城市", city_cb, NULL);

    /* 大字区：温度 + 天气状况 */
    lv_obj_t *big = lv_obj_create(body);
    lv_obj_set_width(big, lv_pct(100));
    lv_obj_set_height(big, 64);
    lv_obj_set_style_bg_color(big, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(big, 1, 0);
    lv_obj_set_style_border_color(big, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(big, 8, 0);
    lv_obj_set_style_pad_all(big, 8, 0);
    lv_obj_set_scrollable(big, false);

    s_temp_lb = lv_label_create(big);
    lv_label_set_text(s_temp_lb, "--");
    lv_obj_set_style_text_font(s_temp_lb, fw_asset_font_24(), 0);
    lv_obj_set_style_text_color(s_temp_lb, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_temp_lb, LV_ALIGN_LEFT_MID, 0, 0);

    s_cond_lb = lv_label_create(big);
    lv_label_set_text(s_cond_lb, "--");
    lv_obj_set_style_text_font(s_cond_lb, fw_asset_font_cn_large(), 0);
    lv_obj_set_style_text_color(s_cond_lb, fw_theme_color_accent(), 0);
    lv_obj_align(s_cond_lb, LV_ALIGN_RIGHT_MID, 0, 0);

    for (int i = 0; i < ROW_COUNT; i++) {
        s_rows[i] = fw_ui_row_btn(body, ROW_SYMBOL[i], ROW_TEXT[i], NULL, NULL);
    }

    fw_ui_row_btn(body, LV_SYMBOL_REFRESH, "刷新", refresh_cb, NULL);
    s_list = fw_ui_list(body, "搜索结果");

    /* 读上次选的城市；没有就让用户先搜 */
    svc_settings_get_str(SET_NS, SET_KEY_CITY, s_city, sizeof(s_city), "");
    svc_settings_get_str(SET_NS, SET_KEY_LAT, s_lat, sizeof(s_lat), "");
    svc_settings_get_str(SET_NS, SET_KEY_LON, s_lon, sizeof(s_lon), "");

    if (s_city[0] != '\0') {
        fw_ui_row_btn_value(s_city_row, s_city);
    } else {
        fw_ui_row_btn_value(s_city_row, "(点此搜索)");
    }

    s_buf = malloc(WX_BUF_SIZE);
    if (s_buf == NULL) {
        set_all_rows("内存不足");
    } else if (s_items == NULL) {
        s_items = malloc(sizeof(geo_item_t) * GEO_MAX);
        if (s_items == NULL) set_all_rows("内存不足");
    }

    if (s_buf != NULL && s_items != NULL && s_city[0] != '\0') {
        refresh_weather();
    } else if (s_buf != NULL && s_items != NULL) {
        set_all_rows("请先选城市");
    }

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created");
    return s_root;
}

static void weather_on_destroy(void *ctx)
{
    (void)ctx;

    lvgl_port_lock(0);
    s_input = NULL;                 /* 随根屏一起删掉，避免悬空 */
    s_input_ta = NULL;
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_city_row = NULL;
    s_temp_lb = NULL;
    s_cond_lb = NULL;
    s_list = NULL;
    for (int i = 0; i < ROW_COUNT; i++) {
        s_rows[i] = NULL;
    }
    lvgl_port_unlock();

    free(s_buf);
    s_buf = NULL;
    free(s_items);
    s_items = NULL;
    s_item_count = 0;
    s_pending = PENDING_NONE;
}

const fw_app_desc_t app_weather_desc = {
    .name = "Weather",
    .title = "天气",
    .icon_64 = &icon_home_weather,
    .symbol = LV_SYMBOL_TINT,
    .on_create = weather_on_create,
    .on_destroy = weather_on_destroy,
};
