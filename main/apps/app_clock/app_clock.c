/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Apps - Clock（APP-CLOCK 时钟）
 *
 * 一个根屏里放三块容器，用显隐切换（不用 fw_window —— 那是给 App 根屏用的）：
 *   主页：大时钟（时:分:秒，按秒刷新）+ 日期 +「时区」「时间设置」入口
 *   设置页：小时钟（校时时能看着秒变）+ 时间格式（12 / 24 小时制）、
 *           立即同步网络时间、校时（±时 / ±分 / ±秒）
 *   时区页：按标签页（常用 / 大洲）切换的全球时区网格，点选即生效并回主页
 * 状态栏返回键 / BOOT 单击：在设置页、时区页回主页，在主页交给框架退出 App。
 *
 * - 12 / 24 小时制放在 svc_time（NVS：sys/clock_24h），状态栏时间也跟着变。
 * - 时区经 svc_time_set_timezone() 设置（内部持久化到 sys/timezone 并发事件）。
 * - 手动校时调 svc_time_set_manual()；没同步过时间时以 2026-01-01 00:00 UTC 为基准，
 *   避免从 1970 起步毫无意义。
 * - 刷新用 100 ms 轮询 + "秒变了才重排文本"，不用 lv_timer_set_period 去对齐相位：
 *   定时器相位那套在 LVGL 里依赖内部 last_run 的改写，不值得为了省几次格式化去赌。
 * - 表里的 UTC 偏移是该时区的标准时间偏移，只用于列表显示；实际时差（含夏令时）由
 *   POSIX TZ 规则算出，所以夏令时期间显示的本地时间会自动跟着变。
 */

#include "app_clock.h"
#include "fw_common.h"
#include "fw_home_icons.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "app.clock";

/* 手动校时的基准：2026-01-01 00:00:00 UTC（未同步时用） */
#define CLK_BASE_TS         1767225600LL
#define CLK_BASE_MIN        1600000000LL

/* 校时步长（秒） */
#define CLK_STEP_HOUR       3600
#define CLK_STEP_MIN        60
#define CLK_STEP_SEC        1

/* 整秒轮询：只在"秒"变化时重排文本，既能秒跳又不会每秒重绘多次 */
#define CLK_TICK_MS         100

/* 当前时区城市名（与 svc_time 的 sys/timezone 并排保存，见 tz_find） */
#define CLK_TZ_NAME_KEY     "timezone_name"

/* 页面内边距（与 fw_ui_page 的内容区一致：12）；页面内行距 6 */
#define CLK_PAD             12
#define CLK_ROW_GAP         6
/* 时区页：标签行高，以及网格里一格的尺寸（2 列刚好铺满 296） */
#define CLK_TAB_H           32
#define CLK_TAB_GAP         6
#define CLK_TZ_ITEM_W       144
#define CLK_TZ_ITEM_H       36

/* ------------------------------- 时区数据 ------------------------------- */

typedef enum {
    TZR_ASIA = 0,
    TZR_EUROPE,
    TZR_AFRICA,
    TZR_AMERICA,
    TZR_OCEANIA,
    TZR_COUNT,
} clock_region_t;

/* 时区页的标签页：0 = 常用（表里 common 标记过的），1.. 依次对应 clock_region_t */
#define TAB_COMMON      0
#define TAB_COUNT       (TZR_COUNT + 1)

static const char *TAB_NAME[TAB_COUNT] = { "常用", "亚洲", "欧洲", "非洲", "美洲", "大洋洲" };

typedef struct {
    const char *name;      /* 显示名（城市 / 地区） */
    const char *tz;        /* POSIX TZ 字符串（含夏令时规则） */
    const char *utc;       /* 标准时间偏移，仅用于列表显示 */
    uint8_t region;        /* clock_region_t */
    bool common;           /* 是否进「常用」标签页 */
} clock_tz_t;

/*
 * 覆盖 UTC-11 ~ UTC+14（含 30 / 45 分时区），组内按偏移从西到东。
 * 时区名用固定偏移的 POSIX 串，有夏令时的用 IDF newlib 支持的标准规则。
 */
static const clock_tz_t TZ_TABLE[] = {
    /* 亚洲 */
    { "德黑兰",     "<+0330>-3:30",                              "UTC+3:30", TZR_ASIA,    false },
    { "迪拜",       "<+04>-4",                                   "UTC+4",    TZR_ASIA,    true  },
    { "喀布尔",     "<+0430>-4:30",                              "UTC+4:30", TZR_ASIA,    false },
    { "卡拉奇",     "PKT-5",                                     "UTC+5",    TZR_ASIA,    false },
    { "塔什干",     "<+05>-5",                                   "UTC+5",    TZR_ASIA,    false },
    { "叶卡捷琳堡", "<+05>-5",                                   "UTC+5",    TZR_ASIA,    false },
    { "新德里",     "IST-5:30",                                  "UTC+5:30", TZR_ASIA,    false },
    { "科伦坡",     "<+0530>-5:30",                              "UTC+5:30", TZR_ASIA,    false },
    { "加德满都",   "<+0545>-5:45",                              "UTC+5:45", TZR_ASIA,    false },
    { "达卡",       "<+06>-6",                                   "UTC+6",    TZR_ASIA,    false },
    { "仰光",       "<+0630>-6:30",                              "UTC+6:30", TZR_ASIA,    false },
    { "曼谷",       "<+07>-7",                                   "UTC+7",    TZR_ASIA,    true  },
    { "河内",       "<+07>-7",                                   "UTC+7",    TZR_ASIA,    false },
    { "雅加达",     "WIB-7",                                     "UTC+7",    TZR_ASIA,    false },
    { "新西伯利亚", "<+07>-7",                                   "UTC+7",    TZR_ASIA,    false },
    { "新加坡",     "<+08>-8",                                   "UTC+8",    TZR_ASIA,    true  },
    { "吉隆坡",     "<+08>-8",                                   "UTC+8",    TZR_ASIA,    false },
    { "马尼拉",     "PST-8",                                     "UTC+8",    TZR_ASIA,    false },
    { "香港",       "HKT-8",                                     "UTC+8",    TZR_ASIA,    false },
    { "台北",       "CST-8",                                     "UTC+8",    TZR_ASIA,    false },
    { "北京",       "CST-8",                                     "UTC+8",    TZR_ASIA,    true  },
    { "乌兰巴托",   "<+08>-8",                                   "UTC+8",    TZR_ASIA,    false },
    { "首尔",       "KST-9",                                     "UTC+9",    TZR_ASIA,    true  },
    { "东京",       "JST-9",                                     "UTC+9",    TZR_ASIA,    true  },
    { "海参崴",     "<+10>-10",                                  "UTC+10",   TZR_ASIA,    false },

    /* 欧洲 */
    { "雷克雅未克", "GMT0",                                      "UTC+0",    TZR_EUROPE,  false },
    { "伦敦",       "GMT0BST,M3.5.0/1,M10.5.0",                  "UTC+0",    TZR_EUROPE,  true  },
    { "都柏林",     "GMT0IST,M3.5.0/1,M10.5.0",                  "UTC+0",    TZR_EUROPE,  false },
    { "里斯本",     "WET0WEST,M3.5.0/1,M10.5.0",                 "UTC+0",    TZR_EUROPE,  false },
    { "UTC",        "UTC0",                                      "UTC",      TZR_EUROPE,  false },
    { "柏林",       "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  true  },
    { "巴黎",       "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  true  },
    { "罗马",       "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "马德里",     "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "阿姆斯特丹", "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "布鲁塞尔",   "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "维也纳",     "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "苏黎世",     "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "华沙",       "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "布拉格",     "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "布达佩斯",   "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "斯德哥尔摩", "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "奥斯陆",     "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "哥本哈根",   "CET-1CEST,M3.5.0,M10.5.0/3",                "UTC+1",    TZR_EUROPE,  false },
    { "雅典",       "EET-2EEST,M3.5.0/3,M10.5.0/4",              "UTC+2",    TZR_EUROPE,  false },
    { "赫尔辛基",   "EET-2EEST,M3.5.0/3,M10.5.0/4",              "UTC+2",    TZR_EUROPE,  false },
    { "基辅",       "EET-2EEST,M3.5.0/3,M10.5.0/4",              "UTC+2",    TZR_EUROPE,  false },
    { "布加勒斯特", "EET-2EEST,M3.5.0/3,M10.5.0/4",              "UTC+2",    TZR_EUROPE,  false },
    { "莫斯科",     "MSK-3",                                     "UTC+3",    TZR_EUROPE,  true  },
    { "伊斯坦布尔", "<+03>-3",                                   "UTC+3",    TZR_EUROPE,  false },

    /* 非洲 */
    { "佛得角",     "<-01>1",                                    "UTC-1",    TZR_AFRICA,  false },
    { "卡萨布兰卡", "<+01>-1",                                   "UTC+1",    TZR_AFRICA,  false },
    { "阿尔及尔",   "CET-1",                                     "UTC+1",    TZR_AFRICA,  false },
    { "突尼斯",     "CET-1",                                     "UTC+1",    TZR_AFRICA,  false },
    { "拉各斯",     "WAT-1",                                     "UTC+1",    TZR_AFRICA,  false },
    { "开罗",       "EET-2EEST,M4.5.5/0,M10.5.4/24",             "UTC+2",    TZR_AFRICA,  false },
    { "的黎波里",   "EET-2",                                     "UTC+2",    TZR_AFRICA,  false },
    { "约翰内斯堡", "SAST-2",                                    "UTC+2",    TZR_AFRICA,  false },
    { "内罗毕",     "EAT-3",                                     "UTC+3",    TZR_AFRICA,  false },
    { "埃塞俄比亚", "EAT-3",                                     "UTC+3",    TZR_AFRICA,  false },
    { "坦桑尼亚",   "EAT-3",                                     "UTC+3",    TZR_AFRICA,  false },
    { "摩加迪沙",   "EAT-3",                                     "UTC+3",    TZR_AFRICA,  false },

    /* 美洲 */
    { "檀香山",     "HST10",                                     "UTC-10",   TZR_AMERICA, false },
    { "安克雷奇",   "AKST9AKDT,M3.2.0,M11.1.0",                  "UTC-9",    TZR_AMERICA, false },
    { "洛杉矶",     "PST8PDT,M3.2.0,M11.1.0",                    "UTC-8",    TZR_AMERICA, true  },
    { "旧金山",     "PST8PDT,M3.2.0,M11.1.0",                    "UTC-8",    TZR_AMERICA, false },
    { "西雅图",     "PST8PDT,M3.2.0,M11.1.0",                    "UTC-8",    TZR_AMERICA, false },
    { "温哥华",     "PST8PDT,M3.2.0,M11.1.0",                    "UTC-8",    TZR_AMERICA, false },
    { "菲尼克斯",   "MST7",                                      "UTC-7",    TZR_AMERICA, false },
    { "丹佛",       "MST7MDT,M3.2.0,M11.1.0",                    "UTC-7",    TZR_AMERICA, false },
    { "盐湖城",     "MST7MDT,M3.2.0,M11.1.0",                    "UTC-7",    TZR_AMERICA, false },
    { "芝加哥",     "CST6CDT,M3.2.0,M11.1.0",                    "UTC-6",    TZR_AMERICA, false },
    { "达拉斯",     "CST6CDT,M3.2.0,M11.1.0",                    "UTC-6",    TZR_AMERICA, false },
    { "休斯敦",     "CST6CDT,M3.2.0,M11.1.0",                    "UTC-6",    TZR_AMERICA, false },
    { "墨西哥城",   "CST6",                                      "UTC-6",    TZR_AMERICA, false },
    { "纽约",       "EST5EDT,M3.2.0,M11.1.0",                    "UTC-5",    TZR_AMERICA, true  },
    { "华盛顿",     "EST5EDT,M3.2.0,M11.1.0",                    "UTC-5",    TZR_AMERICA, false },
    { "波士顿",     "EST5EDT,M3.2.0,M11.1.0",                    "UTC-5",    TZR_AMERICA, false },
    { "迈阿密",     "EST5EDT,M3.2.0,M11.1.0",                    "UTC-5",    TZR_AMERICA, false },
    { "多伦多",     "EST5EDT,M3.2.0,M11.1.0",                    "UTC-5",    TZR_AMERICA, false },
    { "利马",       "<-05>5",                                    "UTC-5",    TZR_AMERICA, false },
    { "波哥大",     "<-05>5",                                    "UTC-5",    TZR_AMERICA, false },
    { "基多",       "<-05>5",                                    "UTC-5",    TZR_AMERICA, false },
    { "加拉加斯",   "<-04>4",                                    "UTC-4",    TZR_AMERICA, false },
    { "拉巴斯",     "<-04>4",                                    "UTC-4",    TZR_AMERICA, false },
    { "圣地亚哥",   "<-04>4<-03>,M9.1.6/24,M4.1.6/24",           "UTC-4",    TZR_AMERICA, false },
    { "圣保罗",     "<-03>3",                                    "UTC-3",    TZR_AMERICA, true  },
    { "里约热内卢", "<-03>3",                                    "UTC-3",    TZR_AMERICA, false },
    { "阿根廷",     "<-03>3",                                    "UTC-3",    TZR_AMERICA, false },
    { "蒙特维多",   "<-03>3",                                    "UTC-3",    TZR_AMERICA, false },
    { "圣约翰斯",   "NST3:30NDT,M3.2.0,M11.1.0",                 "UTC-3:30", TZR_AMERICA, false },
    { "费尔南多",   "<-02>2",                                    "UTC-2",    TZR_AMERICA, false },

    /* 大洋洲 */
    { "帕果帕果",   "SST11",                                     "UTC-11",   TZR_OCEANIA, false },
    { "珀斯",       "AWST-8",                                    "UTC+8",    TZR_OCEANIA, false },
    { "达尔文",     "ACST-9:30",                                 "UTC+9:30", TZR_OCEANIA, false },
    { "阿德莱德",   "ACST-9:30ACDT,M10.1.0,M4.1.0/3",            "UTC+9:30", TZR_OCEANIA, false },
    { "布里斯班",   "AEST-10",                                   "UTC+10",   TZR_OCEANIA, false },
    { "悉尼",       "AEST-10AEDT,M10.1.0,M4.1.0/3",              "UTC+10",   TZR_OCEANIA, true  },
    { "墨尔本",     "AEST-10AEDT,M10.1.0,M4.1.0/3",              "UTC+10",   TZR_OCEANIA, false },
    { "关岛",       "ChST-10",                                   "UTC+10",   TZR_OCEANIA, false },
    { "莫尔兹比港", "<+10>-10",                                  "UTC+10",   TZR_OCEANIA, false },
    { "努美阿",     "<+11>-11",                                  "UTC+11",   TZR_OCEANIA, false },
    { "所罗门",     "<+11>-11",                                  "UTC+11",   TZR_OCEANIA, false },
    { "奥克兰",     "NZST-12NZDT,M9.5.0,M4.1.0/3",               "UTC+12",   TZR_OCEANIA, false },
    { "斐济",       "<+12>-12",                                  "UTC+12",   TZR_OCEANIA, false },
    { "汤加",       "<+13>-13",                                  "UTC+13",   TZR_OCEANIA, false },
    { "基里巴斯",   "<+14>-14",                                  "UTC+14",   TZR_OCEANIA, false },
};
#define TZ_COUNT (sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]))

/* --------------------------------- 状态 --------------------------------- */

typedef enum { PAGE_MAIN = 0, PAGE_SET, PAGE_TZ, PAGE_COUNT } clock_page_t;

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_page[PAGE_COUNT] = { NULL };
static lv_obj_t *s_tab_btn[TAB_COUNT] = { NULL };
static lv_obj_t *s_tz_box = NULL;              /* 时区网格容器（可滚动） */
static lv_obj_t *s_time_lb = NULL;             /* 主页大时钟 */
static lv_obj_t *s_set_time_lb = NULL;         /* 设置页小时钟（校时时看） */
static lv_obj_t *s_date_lb = NULL;
static lv_obj_t *s_tz_row = NULL;              /* 主页：时区入口 */
static lv_obj_t *s_set_row = NULL;             /* 主页：时间设置入口 */
static lv_obj_t *s_fmt_row = NULL;             /* 设置页：时间格式 */
static lv_obj_t *s_sync_row = NULL;            /* 设置页：立即同步 */
static lv_timer_t *s_timer = NULL;
static clock_page_t s_page_cur = PAGE_MAIN;
static uint8_t s_tab = TAB_COMMON;
static int s_tz_idx = -1;                      /* 时区在表里的下标，表外为 -1 */
static bool s_24h = true;
static bool s_foreground = false;              /* 去重：on_start / on_resume 都会恢复 */
static int64_t s_last_sec = 0;
static bool s_tick_logged = false;

/* 行右侧文本的上次值：没变就不写标签，避免每秒无谓重绘 */
static char s_cache_tz[32] = { 0 };
static char s_cache_state[16] = { 0 };
static char s_cache_fmt[16] = { 0 };

/* -------------------------------- 小工具 -------------------------------- */

static void notify(const char *msg)
{
    /* fw_ui_toast 自己保证同一时刻只有一个 Toast（新的顶掉旧的） */
    fw_ui_toast(msg, 1500);
}

static void row_value(lv_obj_t *row, char *cache, size_t cache_len, const char *value)
{
    if (row == NULL || value == NULL) return;
    if (strcmp(cache, value) == 0) return;
    if (fw_ui_row_btn_value(row, value) == ESP_OK) {
        strlcpy(cache, value, cache_len);
    }
}

static const char *weekday_cn(int w)
{
    static const char *wd[] = { "日", "一", "二", "三", "四", "五", "六" };
    return (w >= 0 && w < 7) ? wd[w] : wd[0];
}

static void tz_text(int idx, char *buf, size_t len)
{
    if (idx < 0 || idx >= (int)TZ_COUNT) {
        snprintf(buf, len, "--");
        return;
    }
    snprintf(buf, len, "%s %s", TZ_TABLE[idx].name, TZ_TABLE[idx].utc);
}

/* 同一个 TZ 串可能对应多个城市（北京 / 台北都是 CST-8），所以先按"名字 + TZ 串"
 * 一起认；认不出来再退回只按 TZ 串认（可能是脚本或其它入口改的时区）。 */
static int tz_find(const char *tz, const char *name)
{
    if (name != NULL && name[0] != '\0' && tz != NULL) {
        for (size_t i = 0; i < TZ_COUNT; i++) {
            if (strcmp(TZ_TABLE[i].name, name) == 0 && strcmp(TZ_TABLE[i].tz, tz) == 0) {
                return (int)i;
            }
        }
    }

    if (tz == NULL) return -1;

    /* 只按 TZ 串认时优先认「常用」里的：CST-8 同时是台北和北京的串，默认该认北京 */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < TZ_COUNT; i++) {
            if (pass == 0 && !TZ_TABLE[i].common) continue;
            if (strcmp(TZ_TABLE[i].tz, tz) == 0) return (int)i;
        }
    }
    return -1;
}

static bool tz_visible(const clock_tz_t *e)
{
    if (s_tab == TAB_COMMON) return e->common;
    return e->region == (clock_region_t)(s_tab - 1);
}

/* -------------------------------- 刷新界面 ------------------------------- */

/* 按当前设置格式化时间（12 小时制带 AM / PM，与状态栏一致） */
static void format_time(char *buf, size_t len)
{
    const int64_t now = svc_time_now();

    if (svc_time_get_24h()) {
        if (svc_time_format(now, "%H:%M:%S", buf, len) != ESP_OK) buf[0] = '\0';
        return;
    }

    char hm[12] = { 0 };
    if (svc_time_format(now, "%I:%M:%S", hm, sizeof(hm)) != ESP_OK) {
        buf[0] = '\0';
        return;
    }

    struct tm tmv;
    const time_t t = (time_t)now;
    const bool pm = (localtime_r(&t, &tmv) != NULL) && (tmv.tm_hour >= 12);
    snprintf(buf, len, "%s %s", hm, pm ? "PM" : "AM");
}

/* 主页大时钟、设置页小时钟、日期一起刷（两个时钟共用同一份文本） */
static void refresh_clock(void)
{
    if (s_time_lb == NULL && s_set_time_lb == NULL && s_date_lb == NULL) return;

    char tb[24] = { 0 };
    char db[64] = { 0 };

    if (!svc_time_is_synced()) {
        strlcpy(tb, "--:--:--", sizeof(tb));
        strlcpy(db, "时间未同步", sizeof(db));
    } else {
        format_time(tb, sizeof(tb));

        struct tm tmv;
        const time_t t = (time_t)svc_time_now();
        if (localtime_r(&t, &tmv) == NULL) {
            strlcpy(db, "时间未同步", sizeof(db));
        } else {
            snprintf(db, sizeof(db), "%04d-%02d-%02d 星期%s",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, weekday_cn(tmv.tm_wday));
        }
    }

    if (s_time_lb != NULL) lv_label_set_text(s_time_lb, tb);
    if (s_set_time_lb != NULL) lv_label_set_text(s_set_time_lb, tb);
    if (s_date_lb != NULL) lv_label_set_text(s_date_lb, db);
}

static void refresh_rows(void)
{
    char buf[32] = { 0 };

    /* 主页只显示时区名（偏移在时区页里看）；名字最长 5 个字，放得下 */
    if (s_tz_idx >= 0) {
        snprintf(buf, sizeof(buf), "%s", TZ_TABLE[s_tz_idx].name);
    } else {
        svc_time_get_timezone(buf, sizeof(buf));   /* 表外时区：直接显示 TZ 串 */
    }
    row_value(s_tz_row, s_cache_tz, sizeof(s_cache_tz), buf);

    const char *state = svc_time_is_synced() ? "已同步" : "未同步";
    row_value(s_set_row, s_cache_state, sizeof(s_cache_state), state);
    row_value(s_sync_row, s_cache_state, sizeof(s_cache_state), state);

    /* 以 svc_time 里的设置为准（别的入口改了也跟得上） */
    row_value(s_fmt_row, s_cache_fmt, sizeof(s_cache_fmt),
              svc_time_get_24h() ? "24 小时" : "12 小时");
}

/* 100 ms 轮询，只在"秒"变化时重排文本（CPU 开销可忽略，秒跳稳定） */
static void timer_cb(lv_timer_t *t)
{
    (void)t;

    if (!s_tick_logged) {
        s_tick_logged = true;
        ESP_LOGI(TAG, "tick timer running");
    }

    const int64_t now = svc_time_now();
    if (now == s_last_sec) return;
    s_last_sec = now;

    refresh_clock();
    refresh_rows();
}

/* ------------------------------- 时区页 ------------------------------- */

static void build_tz_grid(bool scroll_to_current);

static void refresh_tabs(void)
{
    for (int i = 0; i < TAB_COUNT; i++) {
        if (s_tab_btn[i] == NULL) continue;

        const bool on = (i == (int)s_tab);
        lv_obj_set_style_bg_color(s_tab_btn[i], on ? fw_theme_color_accent() : fw_theme_color_bg_card(), 0);
        lv_obj_set_style_border_color(s_tab_btn[i], on ? fw_theme_color_accent() : fw_theme_color_border(), 0);

        lv_obj_t *lb = lv_obj_get_child(s_tab_btn[i], 0);
        if (lb != NULL) {
            lv_obj_set_style_text_color(lb, on ? lv_color_white() : fw_theme_color_text_primary(), 0);
        }
    }
}

static void tab_cb(lv_event_t *e)
{
    const uint8_t tab = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (tab == s_tab) return;

    s_tab = tab;
    refresh_tabs();
    build_tz_grid(false);
}

static void show_page(clock_page_t page);

static void tz_pick_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)TZ_COUNT) return;

    if (svc_time_set_timezone(TZ_TABLE[idx].tz) != ESP_OK) {
        notify("设置失败");
        return;
    }

    /* 记住选的是哪个城市：同一个 TZ 串对应多个城市（北京 / 台北都是 CST-8），
     * 只靠 TZ 串认会认到表里靠前的那个，重启 / 换主题后就"跳到"别的城市了 */
    svc_settings_set_str("sys", CLK_TZ_NAME_KEY, TZ_TABLE[idx].name);

    s_tz_idx = idx;

    char buf[40];
    snprintf(buf, sizeof(buf), "时区已切换到%s", TZ_TABLE[idx].name);
    notify(buf);
    show_page(PAGE_MAIN);}

static lv_obj_t *make_tz_item(lv_obj_t *parent, int idx)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, CLK_TZ_ITEM_W, CLK_TZ_ITEM_H);
    lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, tz_pick_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)idx);

    char buf[32];
    tz_text(idx, buf, sizeof(buf));

    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, buf);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, CLK_TZ_ITEM_W - 12);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lv_obj_center(lb);
    return btn;
}

static void build_tz_grid(bool scroll_to_current)
{
    if (s_tz_box == NULL) return;

    lv_obj_clean(s_tz_box);

    lv_obj_t *cur = NULL;
    for (size_t i = 0; i < TZ_COUNT; i++) {
        if (!tz_visible(&TZ_TABLE[i])) continue;

        lv_obj_t *btn = make_tz_item(s_tz_box, (int)i);
        if ((int)i != s_tz_idx) continue;

        /* 当前时区：描边与文字都用强调色标出来 */
        lv_obj_set_style_border_color(btn, fw_theme_color_accent(), 0);
        lv_obj_t *lb = lv_obj_get_child(btn, 0);
        if (lb != NULL) lv_obj_set_style_text_color(lb, fw_theme_color_accent(), 0);
        cur = btn;
    }

    /* 打开时滚到当前时区，省得用户在大洲里翻 */
    if (scroll_to_current && cur != NULL) {
        lv_obj_update_layout(s_tz_box);
        lv_obj_scroll_to_view(cur, LV_ANIM_OFF);
    }
}

static void show_page(clock_page_t page)
{
    s_page_cur = page;

    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_page[i] != NULL) lv_obj_set_hidden(s_page[i], i != (int)page);
    }

    if (page == PAGE_TZ) {
        /* 进时区页默认停到当前时区所在的大洲 */
        if (s_tz_idx >= 0) s_tab = (uint8_t)(TZ_TABLE[s_tz_idx].region + 1);
        refresh_tabs();
        build_tz_grid(true);
    }

    refresh_clock();
    refresh_rows();
}

/* -------------------------------- 事件回调 ------------------------------- */

static void tz_row_cb(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_TZ);
}

static void set_row_cb(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_SET);
}

/* 12 / 24 小时制切换（状态栏时间跟着变） */
static void fmt_cb(lv_event_t *e)
{
    (void)e;

    s_24h = !s_24h;
    if (svc_time_set_24h(s_24h) != ESP_OK) {
        notify("设置失败");
    }
    refresh_rows();
    refresh_clock();
}

/* 立即同步网络时间（联网后自动校时 + 定时同步由 svc_time 负责，这里是手动触发） */
static void sync_cb(lv_event_t *e)
{
    (void)e;

    if (svc_time_sync_ntp() == ESP_OK) {
        notify("正在同步网络时间");
    } else {
        notify("未连接网络");
    }
    refresh_rows();
}

/* 手动校时：user_data 为要加减的秒数（可为负） */
static void adjust_cb(lv_event_t *e)
{
    const int64_t delta = (int64_t)(intptr_t)lv_event_get_user_data(e);

    int64_t base = svc_time_now();
    if (!svc_time_is_synced() || base < CLK_BASE_MIN) {
        base = CLK_BASE_TS;
    }

    if (svc_time_set_manual(base + delta) == ESP_OK) {
        s_last_sec = 0;             /* 时间被改了，下一次轮询立刻重排 */
        refresh_clock();
        refresh_rows();
    } else {
        notify("设置失败");
    }
}

/* -------------------------------- 页面搭建 ------------------------------- */

static lv_obj_t *make_page(lv_obj_t *host)
{
    lv_obj_t *page = lv_obj_create(host);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_row(page, CLK_ROW_GAP, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(page, false);
    return page;
}

/* 居中的时间标签（主页 32 px / 设置页 20 px） */
static lv_obj_t *make_time_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, "--:--:--");
    lv_obj_set_width(lb, lv_pct(100));
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lb, font, 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    return lb;
}

/* 校时行：6 个小按钮，一行放下（296 = 6 * 46 + 5 * 4） */
static lv_obj_t *make_adjust_row(lv_obj_t *parent)
{
    static const struct { const char *text; int32_t step; } BTN[] = {
        { "-时", -CLK_STEP_HOUR }, { "+时", CLK_STEP_HOUR },
        { "-分", -CLK_STEP_MIN },  { "+分", CLK_STEP_MIN },
        { "-秒", -CLK_STEP_SEC },  { "+秒", CLK_STEP_SEC },
    };

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < sizeof(BTN) / sizeof(BTN[0]); i++) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_size(btn, 46, 36);
        lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, adjust_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)BTN[i].step);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, BTN[i].text);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_obj_center(lb);
    }
    return row;
}

static void build_main_page(lv_obj_t *page)
{
    s_time_lb = make_time_label(page, fw_asset_font_32());

    s_date_lb = lv_label_create(page);
    lv_label_set_text(s_date_lb, "");
    lv_obj_set_width(s_date_lb, lv_pct(100));
    lv_obj_set_style_text_align(s_date_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_date_lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(s_date_lb, fw_theme_color_text_secondary(), 0);

    s_tz_row = fw_ui_row_btn(page, LV_SYMBOL_GPS, "时区", tz_row_cb, NULL);
    s_set_row = fw_ui_row_btn(page, LV_SYMBOL_SETTINGS, "时间设置", set_row_cb, NULL);
}

static void build_set_page(lv_obj_t *page)
{
    /* 校时时能看着秒变，所以设置页也放一个小时间 */
    s_set_time_lb = make_time_label(page, fw_asset_font_20());

    s_fmt_row = fw_ui_row_btn(page, LV_SYMBOL_LOOP, "时间格式", fmt_cb, NULL);
    s_sync_row = fw_ui_row_btn(page, LV_SYMBOL_REFRESH, "立即同步", sync_cb, NULL);
    make_adjust_row(page);
}

static void build_tz_page(lv_obj_t *page)
{
    /* 标签行：6 个 46 px 按钮 + 5 个 4 px 间距 = 296 */
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_size(row, lv_pct(100), CLK_TAB_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_size(btn, 46, CLK_TAB_H);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, tab_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, TAB_NAME[i]);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        /* 选中态由 refresh_tabs() 覆盖；这里先给未选中的颜色，不能依赖 LVGL 主题 */
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_obj_center(lb);

        s_tab_btn[i] = btn;
    }

    s_tz_box = lv_obj_create(page);
    lv_obj_set_width(s_tz_box, lv_pct(100));
    lv_obj_set_height(s_tz_box, lv_display_get_vertical_resolution(lv_display_get_default())
                                - FW_STATUSBAR_H - 2 * CLK_PAD - CLK_TAB_H - CLK_TAB_GAP);
    lv_obj_set_style_bg_opa(s_tz_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tz_box, 0, 0);
    lv_obj_set_style_pad_all(s_tz_box, 0, 0);
    lv_obj_set_style_pad_column(s_tz_box, 8, 0);
    lv_obj_set_style_pad_row(s_tz_box, 8, 0);
    lv_obj_set_flex_flow(s_tz_box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_tz_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(s_tz_box, LV_SCROLLBAR_MODE_AUTO);

    refresh_tabs();
}

/* ------------------------------- 生命周期 ------------------------------- */

static void *clock_on_create(void)
{
    lvgl_port_lock(0);

    /* 读持久化设置 */
    s_24h = svc_time_get_24h();
    char tz[32] = { 0 };
    char tz_name[16] = { 0 };
    svc_time_get_timezone(tz, sizeof(tz));
    svc_settings_get_str("sys", CLK_TZ_NAME_KEY, tz_name, sizeof(tz_name), "");
    s_tz_idx = tz_find(tz, tz_name);
    s_tab = (s_tz_idx >= 0) ? (uint8_t)(TZ_TABLE[s_tz_idx].region + 1) : TAB_COMMON;

    s_cache_tz[0] = '\0';
    s_cache_state[0] = '\0';
    s_cache_fmt[0] = '\0';
    s_foreground = false;
    s_last_sec = 0;
    s_tick_logged = false;

    lv_obj_t *host = NULL;
    s_root = fw_ui_page(&host);

    for (int i = 0; i < PAGE_COUNT; i++) {
        s_page[i] = make_page(host);
    }
    build_main_page(s_page[PAGE_MAIN]);
    build_set_page(s_page[PAGE_SET]);
    build_tz_page(s_page[PAGE_TZ]);

    /* 100 ms 轮询 + "秒变了才重排"（见文件头说明，不用 set_period 对齐相位）。
     * 先暂停：等进入前台（on_start / on_resume）再跑 —— 换主题重建时后台 App 也会
     * 走 on_create，后台就没必要每秒醒一次。 */
    s_timer = lv_timer_create(timer_cb, CLK_TICK_MS, NULL);
    if (s_timer != NULL) {
        lv_timer_pause(s_timer);
    }

    show_page(PAGE_MAIN);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "created (24h=%d tz=%s idx=%d timer=%p)",
             s_24h ? 1 : 0, tz, s_tz_idx, (void *)s_timer);
    return s_root;
}

static void clock_on_pause(void *ctx)
{
    (void)ctx;
    s_foreground = false;
    if (s_timer != NULL) lv_timer_pause(s_timer);
    ESP_LOGI(TAG, "paused");
}

/* on_start 与 on_resume 都挂这个函数：paused 时把刷新定时器停了，重新进入前台
 * 可能是 on_start（从桌面点进来）也可能是 on_resume（从返回栈回来），两边都要恢复。
 * 框架在"从桌面重新进入"时两个都会调，用一个标志去重，别做两遍。 */
static void clock_on_resume(void *ctx)
{
    (void)ctx;
    if (s_foreground) return;

    s_foreground = true;
    if (s_timer != NULL) lv_timer_resume(s_timer);
    s_last_sec = 0;                 /* 回来立刻重排一次 */
    refresh_clock();
    refresh_rows();
    ESP_LOGI(TAG, "resumed");
}

static void clock_on_destroy(void *ctx)
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
    s_time_lb = NULL;
    s_set_time_lb = NULL;
    s_date_lb = NULL;
    s_tz_row = NULL;
    s_set_row = NULL;
    s_fmt_row = NULL;
    s_sync_row = NULL;
    s_tz_box = NULL;
    for (int i = 0; i < PAGE_COUNT; i++) s_page[i] = NULL;
    for (int i = 0; i < TAB_COUNT; i++) s_tab_btn[i] = NULL;
    lvgl_port_unlock();
}

/* 返回键：子页面先回主页，主页交给框架退出 App */
static bool clock_on_back(void *ctx)
{
    (void)ctx;

    if (s_page_cur == PAGE_MAIN) return false;

    show_page(PAGE_MAIN);
    return true;
}

const fw_app_desc_t app_clock_desc = {
    .name = "Clock",
    .title = "时钟",
    .icon_64 = &icon_home_clock,
    .symbol = LV_SYMBOL_BELL,
    .on_create = clock_on_create,
    .on_start = clock_on_resume,
    .on_pause = clock_on_pause,
    .on_resume = clock_on_resume,
    .on_destroy = clock_on_destroy,
    .on_back = clock_on_back,
};
