/*
 * SPDX-FileCopyrightText: 2026 SZPI-OS
 *
 * Framework - Script 实现
 *
 * 运行时、脚本管理与能力绑定都在这里。为隔离起见每次运行都新建一个 lua_State，
 * 停止 / 异常时 lua_close，脚本里的定时器随状态一起回收。
 *
 * 沙箱：只打开 base / string / table / math / utf8（不开 io / os / package / debug），
 * 需要系统能力的走绑定模块。
 *
 * 绑定现状（脚本侧模块名）：`ui.page` / `ui.label` / `ui.row` / `ui.image` /
 * `ui.progress` / `ui.toast`（控件带 `set_text` / `set_value` / `set_src` / `set` 方法；
 * `ui.page` 建好页面后会切屏显示，停止 / 出错时自动切回脚本运行前的那个 App 屏）、
 * `sys.*`（含 `sys.mem`）、`timer.every`、`io.*`（GPIO / PWM / I2C / ADC / UART）、
 * `file.*`、`audio.*`（播放 / 提示音 / 录音 / 外部 PCM 流）、`net.http_get`、`event.subscribe` / `event.publish`
 * （固定事件名见 EVENT_MAP，含 BOOT 键 "key" 与 `user.` 开头的自定义事件）、`input.on_click`
 * （= 订阅触摸事件）、`bt.*`（`adv` / `scan_*` / `notify` / `is_connected`
 * 以及 HID：`hid_key(usage[,modifier])` / `hid_mouse(buttons,dx,dy)` / `hid_consumer(usage)`，
 * HID 需主机配对完成才生效，未就绪返回 false）、`camera.open` / `close` / `is_open` /
 * `capture(path)`（拍一张存 TF 卡 —— 帧数据太大，不适合直接交给 Lua）、
 * `mqtt.connect` / `publish` / `subscribe` / `disconnect` / `is_connected`、
 * `ws.connect` / `send` / `is_connected` / `close`、
 * `imu.read` / `is_moving` / `orientation`、
 * `settings.get` / `set` / `get_int` / `set_int`。
 *
 * 能力裁剪（PRD SCR-006）：脚本头部可以写 `-- @perm io,file,net` 声明要用的模块，
 * 写了声明就只注册列出的模块（未声明的模块在脚本里是 nil，调用即 Lua 报错）；
 * 不写声明表示全部可用，兼容内置示例脚本。`ui` / `sys` / `timer` 是基础能力，
 * 始终注册 —— 否则脚本连报错提示都发不出来。解析见 read_perm()。
 *
 * 脚本配置：`settings.*` 统一写进 NVS 的 `"script"` 命名空间，key 原样使用
 * （完整标识是 ns="script" + key，与 sys / net / audio / theme / app_* 隔离）。
 * NVS 的 key 上限 15 个字符，超长时 set 返回 false、get 取到默认值。
 *
 * 首次启动会把内置示例脚本释放到脚本目录（同名不覆盖，无 TF 卡时仅告警）。
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "fw.script";

#define SCRIPT_TASK_STACK   8192
#define SCRIPT_TASK_PRIO    4
#define SCRIPT_TICK_MS      20
#define SCRIPT_TIMER_MAX    8
#define SCRIPT_META_LINES   20
/* 单次 Lua 调用的指令预算：防止脚本死循环把系统拖住（超了当脚本错误处理） */
#define SCRIPT_INSN_BUDGET  20000000L
#define SCRIPT_HTTP_BUF     8192
#define SCRIPT_I2C_READ_MAX 64
#define SCRIPT_UART_READ_MAX 256
#define SCRIPT_UART_TIMEOUT_DEFAULT 1000
#define SCRIPT_RECORD_SEC_DEFAULT   60
#define SCRIPT_RECORD_SEC_MAX       3600

typedef enum { CMD_RUN = 1, CMD_STOP } script_cmd_t;

typedef struct {
    int ref;                    /* LUA_REGISTRYINDEX 里的函数引用；LUA_NOREF = 空闲 */
    uint32_t period_ms;
    int64_t next_us;
} script_timer_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static lua_State *s_L = NULL;
static bool s_running = false;
static char s_current[FW_SCRIPT_NAME_MAX] = { 0 };
static char s_pending_path[FW_SCRIPT_PATH_MAX] = { 0 };
static script_timer_t s_timers[SCRIPT_TIMER_MAX];

/* 界面提示（自动加 LVGL 锁），下面几处都用得到 */
static void toast(const char *msg);

/* ------------------------------- 绑定：ui ------------------------------- */

#define SCRIPT_WIDGET_MT    "szpi.widget"
#define SCRIPT_CB_QUEUE_LEN 16

static lv_obj_t *s_page = NULL;             /* 脚本的页面根，停止时整体删除 */
static lv_obj_t *s_prev_screen = NULL;      /* 脚本界面之前正在显示的屏，停止时还回去 */
static QueueHandle_t s_cb_queue = NULL;     /* LVGL 事件 -> 脚本任务 */

/* 控件被点击时把注册时记下的 Lua 函数引用投给脚本任务（Lua 状态不是线程安全的，
 * 不能在 LVGL 任务里直接调 Lua） */
static void widget_event_cb(lv_event_t *e)
{
    const int ref = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (ref <= 0 || s_cb_queue == NULL) return;

    const int r = ref;
    xQueueSend(s_cb_queue, &r, 0);          /* 满就丢，绝不阻塞 LVGL 任务 */
}

static int push_widget(lua_State *L, lv_obj_t *obj)
{
    lv_obj_t **w = lua_newuserdatauv(L, sizeof(lv_obj_t *), 0);
    *w = obj;
    luaL_setmetatable(L, SCRIPT_WIDGET_MT);
    return 1;
}

static lv_obj_t *check_widget(lua_State *L, int idx)
{
    lv_obj_t **w = luaL_checkudata(L, idx, SCRIPT_WIDGET_MT);
    return *w;
}

static int l_ui_toast(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);

    lvgl_port_lock(0);
    fw_ui_toast(msg, 3000);
    lvgl_port_unlock();
    return 0;
}

/* 建页面：一个脚本只留一个页面，重复调用会先把旧的删掉。
 *
 * 建好之后必须切过去：页面只是内存里的对象，不切屏屏幕上看不到任何东西。
 * 切之前先记下"脚本界面之前"正在显示的屏（正常路径就是脚本管理器 Scripts 的列表根屏），
 * 停止 / 出错时由 teardown_script() 切回去。
 *
 * fw_window 的 s_active 是裸指针：切屏前先 fw_window_sync_active() 和 LVGL 实际活动屏
 * 对齐，免得旧屏删除后地址被新屏复用、被 fw_window_switch_to() 误判成"已在目标屏"。 */
static int l_ui_page(lua_State *L)
{
    lv_obj_t *content = NULL;

    lvgl_port_lock(0);

    fw_window_sync_active();

    /* 只要当前前台不是本脚本的页，就把它记为回退目标。分三种情况：
     *   - 第一次建页：前台是启动脚本的那个 App（正常为脚本管理器 Scripts）。
     *   - 重复建页且脚本页仍在前台：不重记，保留最初的那个 App 屏。
     *   - 脚本页已不在前台（运行中换过主题、或用户按了返回键）：重记当前前台，
     *     这样回退目标始终是个活的屏，避免旧指针悬空后按地址复用误切。
     * 记完在同一次加锁内立刻切屏，中间不会有别的任务改活动屏。 */
    if (lv_screen_active() != s_page) {
        s_prev_screen = lv_screen_active();
    }

    lv_obj_t *page = fw_ui_page(&content);

    if (page != NULL) {
        lv_obj_t *old = s_page;
        s_page = page;
        /* 先切到新页再删旧页：删除正在显示的屏会把 display 的 act_scr 置空，
         * 之后任何 lv_screen_load* 都会空指针崩溃（AGENTS 4.12）。 */
        fw_window_switch_to(page, LV_SCR_LOAD_ANIM_NONE, 0);
        if (old != NULL) {
            lv_obj_delete(old);
        }
    }

    lvgl_port_unlock();

    if (page == NULL || content == NULL) {
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, content);
}

static int l_ui_label(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const char *text = luaL_checkstring(L, 2);

    lvgl_port_lock(0);
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, text);
    lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
    lvgl_port_unlock();

    return push_widget(L, lb);
}

static int l_ui_row(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const char *symbol = luaL_checkstring(L, 2);
    const char *text = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    lua_pushvalue(L, 4);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lvgl_port_lock(0);
    lv_obj_t *btn = fw_ui_row_btn(parent, symbol, text, widget_event_cb, NULL);
    if (btn != NULL) {
        lv_obj_set_user_data(btn, (void *)(intptr_t)ref);
    }
    lvgl_port_unlock();

    if (btn == NULL) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, btn);
}

/* 图片：路径先经 fw_asset_fs_path() 转成 LVGL 认的 "A:/sdcard/..." 再交给 lv_image_set_src()，
 * 解码由 LVGL 自己读文件（fw_asset 注册的 'A' 盘驱动）。换图用控件的 set_src(path) 方法。 */
static int l_ui_image(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const char *path = luaL_checkstring(L, 2);

    char fs_path[FW_SCRIPT_PATH_MAX + 8];
    if (fw_asset_fs_path(path, fs_path, sizeof(fs_path)) != ESP_OK) {
        ESP_LOGW(TAG, "image path too long: %s", path);
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *img = lv_image_create(parent);
    if (img != NULL) {
        lv_image_set_src(img, fs_path);     /* LVGL 内部会复制这份路径 */
    }
    lvgl_port_unlock();

    if (img == NULL) {
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, img);
}

/* 进度条：直接复用通用组件（返回的是外层卡片，set(pct) 由 fw_ui_progress_set 更新内部 bar） */
static int l_ui_progress(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const char *title = luaL_checkstring(L, 2);

    lvgl_port_lock(0);
    lv_obj_t *bar = fw_ui_progress_bar(parent, title);
    lvgl_port_unlock();

    if (bar == NULL) {
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, bar);
}

static int l_widget_set_text(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const char *text = luaL_checkstring(L, 2);

    lvgl_port_lock(0);
    if (obj != NULL && lv_obj_is_valid(obj)) {
        lv_label_set_text(obj, text);
    }
    lvgl_port_unlock();
    return 0;
}

static int l_widget_set_value(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const char *text = luaL_checkstring(L, 2);

    lvgl_port_lock(0);
    if (obj != NULL && lv_obj_is_valid(obj)) {
        fw_ui_row_btn_value(obj, text);
    }
    lvgl_port_unlock();
    return 0;
}

/* 换图：方法表是所有控件共用的，所以先确认 obj 真的是 image，别的控件调用直接忽略 */
static int l_widget_set_src(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const char *path = luaL_checkstring(L, 2);

    char fs_path[FW_SCRIPT_PATH_MAX + 8];
    if (fw_asset_fs_path(path, fs_path, sizeof(fs_path)) != ESP_OK) {
        return 0;
    }

    lvgl_port_lock(0);
    if (obj != NULL && lv_obj_is_valid(obj) && lv_obj_check_type(obj, &lv_image_class)) {
        lv_image_set_src(obj, fs_path);
    }
    lvgl_port_unlock();
    return 0;
}

/* 进度 0-100：方法表是所有控件共用的，先按"标题 label + 进度条 bar"的结构
 * 确认这是 fw_ui_progress_bar() 建出来的卡片，别的控件调用直接忽略 */
static int l_widget_set(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    lua_Integer pct = luaL_checkinteger(L, 2);
    if (pct < 0) pct = 0;

    lvgl_port_lock(0);
    if (obj != NULL && lv_obj_is_valid(obj)) {
        lv_obj_t *inner = lv_obj_get_child(obj, 1);
        if (inner != NULL && lv_obj_check_type(inner, &lv_bar_class)) {
            fw_ui_progress_set(obj, (uint8_t)pct);  /* 内部把 > 100 截到 100 */
        }
    }
    lvgl_port_unlock();
    return 0;
}

static const luaL_Reg widget_methods[] = {
    { "set_text", l_widget_set_text },
    { "set_value", l_widget_set_value },
    { "set_src", l_widget_set_src },
    { "set", l_widget_set },
    { NULL, NULL },
};

static const luaL_Reg ui_lib[] = {
    { "toast", l_ui_toast },
    { "page", l_ui_page },
    { "label", l_ui_label },
    { "row", l_ui_row },
    { "image", l_ui_image },
    { "progress", l_ui_progress },
    { NULL, NULL },
};

static void widget_create_metatable(lua_State *L)
{
    luaL_newmetatable(L, SCRIPT_WIDGET_MT);
    luaL_setfuncs(L, widget_methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");     /* 方法表即元表本身 */
    lua_pop(L, 1);
}

static int luaopen_ui(lua_State *L)
{
    widget_create_metatable(L);
    luaL_newlib(L, ui_lib);
    return 1;
}

/* 在脚本任务里消费控件事件 */
static void drain_widget_events(void)
{
    if (s_L == NULL || s_cb_queue == NULL) return;

    int ref = 0;
    while (xQueueReceive(s_cb_queue, &ref, 0) == pdTRUE) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "widget callback error: %s", lua_tostring(s_L, -1));
            lua_pop(s_L, 1);
        }
    }
}

/* ------------------------------ 绑定：event ------------------------------ */

#define SCRIPT_EVENT_MAX    8

typedef struct {
    const char *name;
    svc_event_id_t id;
} script_event_map_t;

static const script_event_map_t EVENT_MAP[] = {
    { "touch",           SVC_EVENT_TOUCH },
    /* BOOT 键：回调参数是 1 字节字符串，string.byte(v) 得到事件序号
     * （0 单击 / 1 双击 / 2 长按 / 3 极长按） */
    { "key",             SVC_EVENT_KEY },
    { "imu_motion",      SVC_EVENT_IMU_MOTION },
    { "imu_orientation", SVC_EVENT_IMU_ORIENTATION },
    { "imu_shake",       SVC_EVENT_IMU_SHAKE },
    { "imu_pickup",      SVC_EVENT_IMU_PICKUP },
    { "wifi_connected",  SVC_EVENT_WIFI_CONNECTED },
    { "wifi_disconnected", SVC_EVENT_WIFI_DISCONNECTED },
    { "bt_state",        SVC_EVENT_BT_STATE_CHANGED },
    { "brightness",      SVC_EVENT_BRIGHTNESS_CHANGED },
    { "sd_mounted",      SVC_EVENT_SD_MOUNTED },
    { "sd_unmounted",    SVC_EVENT_SD_UNMOUNTED },
    { "audio_record_finished", SVC_EVENT_AUDIO_RECORD_FINISHED },
    { "script_started",  SVC_EVENT_SCRIPT_STARTED },
    { "script_stopped",  SVC_EVENT_SCRIPT_STOPPED },
};

/* 脚本自定义事件：名字以 "user." 开头时映射到 SVC_EVENT_USER_BASE + hash(name)。
 * 哈希用 FNV-1a 32 位再取低 12 位：纯计算、无随机种子、不依赖运行顺序，因此
 * 同一字符串在同一固件内稳定映射到同一事件号 —— 发布方与订阅方总能看到同一个
 * 事件。不同名字有小概率撞进同一个槽（4096 个槽），脚本侧自行避开即可。 */
#define SCRIPT_USER_EVENT_MASK  0x0FFF
#define SCRIPT_USER_EVENT_PREFIX "user."

static uint32_t user_event_hash(const char *name)
{
    uint32_t h = 2166136261u;               /* FNV-1a offset basis */
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;                     /* FNV-1a prime */
    }
    return h & SCRIPT_USER_EVENT_MASK;
}

/* 订阅登记：停止脚本时要按 id 退订，否则回调会打到已经关掉的 Lua 状态上 */
typedef struct {
    svc_event_id_t id;
    int ref;
} script_sub_t;

typedef struct {
    int ref;
    uint32_t len;
    uint8_t data[32];
} script_bus_evt_t;

static script_sub_t s_subs[SCRIPT_EVENT_MAX];
static QueueHandle_t s_bus_queue = NULL;

static void subs_reset(void)
{
    for (int i = 0; i < SCRIPT_EVENT_MAX; i++) {
        s_subs[i].id = SVC_EVENT_BASE;
        s_subs[i].ref = LUA_NOREF;
    }
}

/* 事件总线回调在 dispatcher 任务里跑：只投递，不碰 Lua */
static void bus_event_cb(const svc_event_t *evt, void *user)
{
    if (s_bus_queue == NULL) return;

    script_bus_evt_t out = { 0 };
    out.ref = (int)(intptr_t)user;
    if (evt->data != NULL && evt->data_len > 0) {
        out.len = (evt->data_len > sizeof(out.data)) ? sizeof(out.data) : evt->data_len;
        memcpy(out.data, evt->data, out.len);
    }
    xQueueSend(s_bus_queue, &out, 0);       /* 满就丢 */
}

/* 先查固定事件名，再认 "user." 前缀的自定义事件（两者不会重名） */
static bool event_id_of(const char *name, svc_event_id_t *out)
{
    const size_t n = sizeof(EVENT_MAP) / sizeof(EVENT_MAP[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(EVENT_MAP[i].name, name) == 0) {
            *out = EVENT_MAP[i].id;
            return true;
        }
    }

    const size_t prefix = sizeof(SCRIPT_USER_EVENT_PREFIX) - 1;
    if (strncmp(name, SCRIPT_USER_EVENT_PREFIX, prefix) == 0 && name[prefix] != '\0') {
        *out = (svc_event_id_t)(SVC_EVENT_USER_BASE + user_event_hash(name));
        return true;
    }
    return false;
}

static int l_event_subscribe(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    svc_event_id_t id;
    if (!event_id_of(name, &id)) {
        return luaL_error(L, "unknown event: %s", name);
    }

    int slot = -1;
    for (int i = 0; i < SCRIPT_EVENT_MAX; i++) {
        if (s_subs[i].ref == LUA_NOREF) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return luaL_error(L, "too many subscriptions (max %d)", SCRIPT_EVENT_MAX);
    }

    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (svc_event_bus_subscribe(id, bus_event_cb, (void *)(intptr_t)ref) != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "subscribe failed: %s", name);
    }

    s_subs[slot].id = id;
    s_subs[slot].ref = ref;
    lua_pushboolean(L, true);
    return 1;
}

static int l_event_publish(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *payload = luaL_optlstring(L, 2, NULL, &len);

    svc_event_id_t id;
    if (!event_id_of(name, &id)) {
        return luaL_error(L, "unknown event: %s", name);
    }

    lua_pushboolean(L, svc_event_bus_publish(id, payload, (uint32_t)len) == ESP_OK);
    return 1;
}

static const luaL_Reg event_lib[] = {
    { "subscribe", l_event_subscribe },
    { "publish", l_event_publish },
    { NULL, NULL },
};

static int luaopen_event(lua_State *L)
{
    luaL_newlib(L, event_lib);
    return 1;
}

/* input.on_click 就是订阅触摸事件（svc_power 已占用 periph_touch 的回调槽，
 * 脚本不能再挂，所以统一走事件总线） */
static int l_input_on_click(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    lua_pushstring(L, "touch");
    lua_insert(L, 1);
    return l_event_subscribe(L);
}

static const luaL_Reg input_lib[] = {
    { "on_click", l_input_on_click },
    { NULL, NULL },
};

static int luaopen_input(lua_State *L)
{
    luaL_newlib(L, input_lib);
    return 1;
}

static void drain_bus_events(void)
{
    if (s_L == NULL || s_bus_queue == NULL) return;

    script_bus_evt_t ev;
    while (xQueueReceive(s_bus_queue, &ev, 0) == pdTRUE) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, ev.ref);
        if (ev.len > 0) {
            lua_pushlstring(s_L, (const char *)ev.data, ev.len);
        } else {
            lua_pushnil(s_L);
        }
        if (lua_pcall(s_L, 1, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "event callback error: %s", lua_tostring(s_L, -1));
            lua_pop(s_L, 1);
        }
    }
}

/* --------------------------- 绑定：mqtt / ws --------------------------- */

/* MQTT / WebSocket 的回调在各自的协议栈任务里跑，而 Lua 状态不是线程安全的：
 * 回调只把「函数引用 + 数据」投进队列，由 script_task 取出后再用 lua_pcall 调用。
 * svc_mqtt / svc_ws 各自只有一个回调槽，所以脚本侧也只保留一个订阅 / 一个连接。 */

#define SCRIPT_MSG_QUEUE_LEN 6
#define SCRIPT_MSG_TOPIC_MAX 80     /* svc_mqtt 内部就是按 80 字节缓存主题 */
#define SCRIPT_MSG_BODY_MAX  192    /* 超出部分截断（MQTT 服务侧本身上限 255） */

typedef struct {
    int ref;                            /* 要调用的 Lua 函数引用 */
    char topic[SCRIPT_MSG_TOPIC_MAX];   /* MQTT 主题；WebSocket 留空串 */
    uint32_t len;                       /* body 里的有效字节数 */
    char body[SCRIPT_MSG_BODY_MAX];
} script_msg_evt_t;

static QueueHandle_t s_msg_queue = NULL;

static int s_ws_ref = LUA_NOREF;
static bool s_ws_active = false;

typedef struct {
    int ref;
    char topic[SCRIPT_MSG_TOPIC_MAX];
} script_mqtt_sub_t;

static script_mqtt_sub_t s_mqtt_sub = { LUA_NOREF, { 0 } };
static bool s_mqtt_active = false;

static void mqtt_msg_cb(const char *topic, const char *payload, size_t len, void *user)
{
    (void)user;
    if (s_msg_queue == NULL || s_mqtt_sub.ref == LUA_NOREF) return;
    if (topic == NULL || strcmp(topic, s_mqtt_sub.topic) != 0) return;  /* 只转发登记的主题 */

    script_msg_evt_t out = { 0 };
    out.ref = s_mqtt_sub.ref;
    snprintf(out.topic, sizeof(out.topic), "%.*s", (int)sizeof(out.topic) - 1, topic);
    if (payload != NULL && len > 0) {
        out.len = (len > sizeof(out.body)) ? (uint32_t)sizeof(out.body) : (uint32_t)len;
        memcpy(out.body, payload, out.len);
    }
    xQueueSend(s_msg_queue, &out, 0);       /* 满就丢，绝不阻塞协议栈任务 */
}

static void ws_msg_cb(const char *data, size_t len, void *user)
{
    (void)user;
    if (s_msg_queue == NULL || s_ws_ref == LUA_NOREF) return;

    script_msg_evt_t out = { 0 };
    out.ref = s_ws_ref;
    if (data != NULL && len > 0) {
        out.len = (len > sizeof(out.body)) ? (uint32_t)sizeof(out.body) : (uint32_t)len;
        memcpy(out.body, data, out.len);
    }
    xQueueSend(s_msg_queue, &out, 0);
}

/* 退订 / 断开脚本建立的 MQTT、WebSocket 连接（停止或启动失败时调用） */
static void external_reset(void)
{
    if (s_mqtt_sub.ref != LUA_NOREF) {
        svc_mqtt_unsubscribe(s_mqtt_sub.topic);
        s_mqtt_sub.ref = LUA_NOREF;
        s_mqtt_sub.topic[0] = '\0';
    }
    if (s_mqtt_active) {
        svc_mqtt_disconnect();
        s_mqtt_active = false;
    }
    s_ws_ref = LUA_NOREF;
    if (s_ws_active) {
        svc_ws_disconnect();
        s_ws_active = false;
    }
}

/* 丢掉回调队列里的旧事件：里面存的是已关闭 Lua 状态的函数引用，绝不能跨脚本使用 */
static void flush_callback_queues(void)
{
    if (s_cb_queue != NULL) {
        int ref = 0;
        while (xQueueReceive(s_cb_queue, &ref, 0) == pdTRUE) { }
    }
    if (s_bus_queue != NULL) {
        script_bus_evt_t evt;
        while (xQueueReceive(s_bus_queue, &evt, 0) == pdTRUE) { }
    }
    if (s_msg_queue != NULL) {
        script_msg_evt_t msg;
        while (xQueueReceive(s_msg_queue, &msg, 0) == pdTRUE) { }
    }
}

/* 在脚本任务里消费 MQTT / WebSocket 消息 */
static void drain_msg_events(void)
{
    if (s_L == NULL || s_msg_queue == NULL) return;

    script_msg_evt_t ev;
    while (xQueueReceive(s_msg_queue, &ev, 0) == pdTRUE) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, ev.ref);
        int nargs = 0;
        if (ev.topic[0] != '\0') {          /* MQTT: fn(topic, payload) */
            lua_pushstring(s_L, ev.topic);
            nargs++;
        }
        lua_pushlstring(s_L, ev.body, ev.len);  /* WebSocket: fn(msg) */
        nargs++;
        if (lua_pcall(s_L, nargs, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "message callback error: %s", lua_tostring(s_L, -1));
            lua_pop(s_L, 1);
        }
    }
}

static int l_mqtt_connect(lua_State *L)
{
    const char *uri = luaL_checkstring(L, 1);
    const char *user = luaL_optstring(L, 2, NULL);
    const char *pass = luaL_optstring(L, 3, NULL);

    const bool ok = (svc_mqtt_connect(uri, user, pass) == ESP_OK);
    if (ok) s_mqtt_active = true;
    lua_pushboolean(L, ok);
    return 1;
}

static int l_mqtt_publish(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);
    const char *payload = luaL_checkstring(L, 2);
    const lua_Integer qos = luaL_optinteger(L, 3, 0);

    lua_pushboolean(L, svc_mqtt_publish(topic, payload, (int)qos) == ESP_OK);
    return 1;
}

static int l_mqtt_subscribe(lua_State *L)
{
    const char *topic = luaL_checkstring(L, 1);

    int qos = 0;
    int fn = 2;
    if (!lua_isfunction(L, 2)) {            /* subscribe(topic, qos, fn) 形式 */
        qos = (int)luaL_checkinteger(L, 2);
        fn = 3;
    }
    luaL_checktype(L, fn, LUA_TFUNCTION);

    /* svc_mqtt 只有一个回调槽，重复订阅要先把上一个退掉并释放引用 */
    if (s_mqtt_sub.ref != LUA_NOREF) {
        svc_mqtt_unsubscribe(s_mqtt_sub.topic);
        luaL_unref(L, LUA_REGISTRYINDEX, s_mqtt_sub.ref);
        s_mqtt_sub.ref = LUA_NOREF;
        s_mqtt_sub.topic[0] = '\0';
    }

    lua_pushvalue(L, fn);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    snprintf(s_mqtt_sub.topic, sizeof(s_mqtt_sub.topic), "%.*s",
             (int)sizeof(s_mqtt_sub.topic) - 1, topic);
    s_mqtt_sub.ref = ref;                   /* 先登记再订阅，避免回调抢先到达时取不到引用 */

    if (svc_mqtt_subscribe(topic, qos, mqtt_msg_cb, NULL) != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        s_mqtt_sub.ref = LUA_NOREF;
        s_mqtt_sub.topic[0] = '\0';
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int l_mqtt_disconnect(lua_State *L)
{
    if (s_mqtt_sub.ref != LUA_NOREF) {
        svc_mqtt_unsubscribe(s_mqtt_sub.topic);
        luaL_unref(L, LUA_REGISTRYINDEX, s_mqtt_sub.ref);
        s_mqtt_sub.ref = LUA_NOREF;
        s_mqtt_sub.topic[0] = '\0';
    }
    if (s_mqtt_active) {
        svc_mqtt_disconnect();
        s_mqtt_active = false;
    }
    return 0;
}

static int l_mqtt_is_connected(lua_State *L)
{
    lua_pushboolean(L, svc_mqtt_is_connected());
    return 1;
}

static const luaL_Reg mqtt_lib[] = {
    { "connect", l_mqtt_connect },
    { "publish", l_mqtt_publish },
    { "subscribe", l_mqtt_subscribe },
    { "disconnect", l_mqtt_disconnect },
    { "is_connected", l_mqtt_is_connected },
    { NULL, NULL },
};

static int luaopen_mqtt(lua_State *L)
{
    luaL_newlib(L, mqtt_lib);
    return 1;
}

static int l_ws_connect(lua_State *L)
{
    const char *uri = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* 同一时刻只维持一个连接：重复 connect 先断旧的。本函数在 script_task 里跑，
     * 不违反 svc_ws "不能在消息回调里 disconnect" 的约束。 */
    if (s_ws_active) {
        svc_ws_disconnect();
        s_ws_active = false;
    }
    if (s_ws_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_ws_ref);
        s_ws_ref = LUA_NOREF;
    }

    lua_pushvalue(L, 2);
    s_ws_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (svc_ws_connect(uri, ws_msg_cb, NULL) != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_ws_ref);
        s_ws_ref = LUA_NOREF;
        lua_pushboolean(L, false);
        return 1;
    }
    s_ws_active = true;
    lua_pushboolean(L, true);
    return 1;
}

static int l_ws_send(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, svc_ws_send(data, len) == ESP_OK);
    return 1;
}

static int l_ws_is_connected(lua_State *L)
{
    lua_pushboolean(L, svc_ws_is_connected());
    return 1;
}

static int l_ws_close(lua_State *L)
{
    if (s_ws_active) {
        svc_ws_disconnect();
        s_ws_active = false;
    }
    if (s_ws_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_ws_ref);
        s_ws_ref = LUA_NOREF;
    }
    return 0;
}

static const luaL_Reg ws_lib[] = {
    { "connect", l_ws_connect },
    { "send", l_ws_send },
    { "is_connected", l_ws_is_connected },
    { "close", l_ws_close },
    { NULL, NULL },
};

static int luaopen_ws(lua_State *L)
{
    luaL_newlib(L, ws_lib);
    return 1;
}

/* ------------------------------- 绑定：imu ------------------------------- */

static int l_imu_read(lua_State *L)
{
    periph_imu_data_t d;
    if (svc_imu_read(&d) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 9);
    lua_pushnumber(L, d.acc_x);
    lua_setfield(L, -2, "acc_x");
    lua_pushnumber(L, d.acc_y);
    lua_setfield(L, -2, "acc_y");
    lua_pushnumber(L, d.acc_z);
    lua_setfield(L, -2, "acc_z");
    lua_pushnumber(L, d.gyr_x);
    lua_setfield(L, -2, "gyr_x");
    lua_pushnumber(L, d.gyr_y);
    lua_setfield(L, -2, "gyr_y");
    lua_pushnumber(L, d.gyr_z);
    lua_setfield(L, -2, "gyr_z");
    lua_pushnumber(L, d.roll);
    lua_setfield(L, -2, "roll");
    lua_pushnumber(L, d.pitch);
    lua_setfield(L, -2, "pitch");
    lua_pushnumber(L, d.yaw);
    lua_setfield(L, -2, "yaw");
    return 1;
}

static int l_imu_is_moving(lua_State *L)
{
    lua_pushboolean(L, svc_imu_is_moving());
    return 1;
}

static int l_imu_orientation(lua_State *L)
{
    const char *name = "portrait";

    switch (svc_imu_get_orientation()) {
    case SVC_IMU_ORIENTATION_PORTRAIT:       name = "portrait"; break;
    case SVC_IMU_ORIENTATION_LANDSCAPE:      name = "landscape"; break;
    case SVC_IMU_ORIENTATION_PORTRAIT_FLIP:  name = "portrait_flip"; break;
    case SVC_IMU_ORIENTATION_LANDSCAPE_FLIP: name = "landscape_flip"; break;
    default: break;
    }

    lua_pushstring(L, name);
    return 1;
}

static const luaL_Reg imu_lib[] = {
    { "read", l_imu_read },
    { "is_moving", l_imu_is_moving },
    { "orientation", l_imu_orientation },
    { NULL, NULL },
};

static int luaopen_imu(lua_State *L)
{
    luaL_newlib(L, imu_lib);
    return 1;
}

/* ----------------------------- 绑定：settings ----------------------------- */

/* 命名空间固定 "script"，key 原样使用（不叠前缀）：写进 NVS 的完整标识是
 * ns="script" + key，与 sys / net / audio / theme / app_* 等天然隔离。
 * NVS 的 key 上限 15 个字符，超长时 set 返回 false、get 取到默认值。 */

#define SCRIPT_SETTINGS_NS       "script"
#define SCRIPT_SETTINGS_VAL_MAX  128

static int l_settings_get(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *def = luaL_optstring(L, 2, NULL);

    char buf[SCRIPT_SETTINGS_VAL_MAX];
    svc_settings_get_str(SCRIPT_SETTINGS_NS, key, buf, sizeof(buf), def);
    lua_pushstring(L, buf);
    return 1;
}

static int l_settings_set(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *val = luaL_checkstring(L, 2);

    lua_pushboolean(L, svc_settings_set_str(SCRIPT_SETTINGS_NS, key, val) == ESP_OK);
    return 1;
}

static int l_settings_get_int(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const lua_Integer def = luaL_optinteger(L, 2, 0);

    int32_t val = (int32_t)def;
    svc_settings_get_i32(SCRIPT_SETTINGS_NS, key, &val, (int32_t)def);
    lua_pushinteger(L, val);
    return 1;
}

static int l_settings_set_int(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const lua_Integer val = luaL_checkinteger(L, 2);

    lua_pushboolean(L, svc_settings_set_i32(SCRIPT_SETTINGS_NS, key, (int32_t)val) == ESP_OK);
    return 1;
}

static const luaL_Reg settings_lib[] = {
    { "get", l_settings_get },
    { "set", l_settings_set },
    { "get_int", l_settings_get_int },
    { "set_int", l_settings_set_int },
    { NULL, NULL },
};

static int luaopen_settings(lua_State *L)
{
    luaL_newlib(L, settings_lib);
    return 1;
}

/* ------------------------------- 绑定：bt ------------------------------- */

static int l_bt_state_name(lua_State *L)
{
    lua_pushstring(L, svc_bt_state_name(svc_bt_get_state()));
    return 1;
}

static int l_bt_is_connected(lua_State *L)
{
    lua_pushboolean(L, svc_bt_is_connected());
    return 1;
}

static int l_bt_adv(lua_State *L)
{
    const bool on = lua_toboolean(L, 1);
    lua_pushboolean(L, (on ? svc_bt_adv_start() : svc_bt_adv_stop()) == ESP_OK);
    return 1;
}

static int l_bt_scan_start(lua_State *L)
{
    lua_Integer sec = 5;
    if (lua_gettop(L) >= 1) sec = luaL_checkinteger(L, 1);
    lua_pushboolean(L, svc_bt_scan_start((uint32_t)sec) == ESP_OK);
    return 1;
}

static int l_bt_scan_stop(lua_State *L)
{
    (void)L;
    svc_bt_scan_stop();
    return 0;
}

static int l_bt_scan_results(lua_State *L)
{
    svc_bt_scan_result_t res[16];
    size_t n = 0;
    if (svc_bt_get_scan_results(res, sizeof(res) / sizeof(res[0]), &n) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, res[i].name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, res[i].rssi);
        lua_setfield(L, -2, "rssi");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

static int l_bt_notify(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    lua_pushboolean(L, svc_bt_notify(data, len) == ESP_OK);
    return 1;
}

/* HID：未配对 / 未连接时服务层返回 ESP_ERR_INVALID_STATE，这里统一反映成 false */
static int l_bt_hid_key(lua_State *L)
{
    const lua_Integer usage = luaL_checkinteger(L, 1);
    const lua_Integer modifier = luaL_optinteger(L, 2, 0);
    if (usage < 0 || usage > 0xFF || modifier < 0 || modifier > 0xFF) {
        lua_pushboolean(L, false);
        return 1;
    }
    /* 一键单击：服务内部自动在 30 ms 后补发松开报告 */
    lua_pushboolean(L, svc_bt_hid_key_click((uint8_t)usage, (uint8_t)modifier) == ESP_OK);
    return 1;
}

static int l_bt_hid_mouse(lua_State *L)
{
    const lua_Integer buttons = luaL_checkinteger(L, 1);
    const lua_Integer dx = luaL_checkinteger(L, 2);
    const lua_Integer dy = luaL_checkinteger(L, 3);
    if (buttons < 0 || buttons > 0xFF || dx < -127 || dx > 127 || dy < -127 || dy > 127) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, svc_bt_hid_mouse((uint8_t)buttons, (int8_t)dx, (int8_t)dy) == ESP_OK);
    return 1;
}

static int l_bt_hid_consumer(lua_State *L)
{
    const lua_Integer usage = luaL_checkinteger(L, 1);
    if (usage < 0 || usage > 0xFF) {
        lua_pushboolean(L, false);
        return 1;
    }
    /* 消费类单击：服务内部自动补发松开 */
    lua_pushboolean(L, svc_bt_hid_consumer_click((uint8_t)usage) == ESP_OK);
    return 1;
}

static const luaL_Reg bt_lib[] = {
    { "state_name", l_bt_state_name },
    { "is_connected", l_bt_is_connected },
    { "adv", l_bt_adv },
    { "scan_start", l_bt_scan_start },
    { "scan_stop", l_bt_scan_stop },
    { "scan_results", l_bt_scan_results },
    { "notify", l_bt_notify },
    { "hid_key", l_bt_hid_key },
    { "hid_mouse", l_bt_hid_mouse },
    { "hid_consumer", l_bt_hid_consumer },
    { NULL, NULL },
};

static int luaopen_bt(lua_State *L)
{
    luaL_newlib(L, bt_lib);
    return 1;
}

/* ------------------------------ 绑定：camera ------------------------------ */

/* 帧数据太大，不适合交给 Lua：只暴露"打开 / 拍照存盘 / 关闭" */
static int l_camera_open(lua_State *L)
{
    const bool jpeg = lua_toboolean(L, 1);
    lua_pushboolean(L, svc_camera_open(jpeg ? SVC_CAMERA_FMT_JPEG : SVC_CAMERA_FMT_RGB565,
                                       SVC_CAMERA_SIZE_QVGA) == ESP_OK);
    return 1;
}

static int l_camera_close(lua_State *L)
{
    (void)L;
    svc_camera_close();
    return 0;
}

static int l_camera_is_open(lua_State *L)
{
    lua_pushboolean(L, svc_camera_is_open());
    return 1;
}

static int l_camera_capture(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    if (!svc_camera_is_open()) {
        lua_pushboolean(L, false);
        return 1;
    }

    svc_camera_frame_t frame = { 0 };
    if (svc_camera_capture(&frame) != ESP_OK) {
        lua_pushboolean(L, false);
        return 1;
    }

    const esp_err_t err = svc_storage_write(path, frame.data, frame.len);
    svc_camera_release();

    lua_pushboolean(L, err == ESP_OK);
    return 1;
}

static const luaL_Reg camera_lib[] = {
    { "open", l_camera_open },
    { "close", l_camera_close },
    { "is_open", l_camera_is_open },
    { "capture", l_camera_capture },
    { NULL, NULL },
};

static int luaopen_camera(lua_State *L)
{
    luaL_newlib(L, camera_lib);
    return 1;
}

/* ------------------------------- 绑定：sys ------------------------------- */

static int l_sys_uptime(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000000));
    return 1;
}

static int l_sys_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)svc_time_now());
    return 1;
}

static int l_sys_localtime(lua_State *L)
{
    const time_t t = (time_t)svc_time_now();
    struct tm tm = { 0 };
    localtime_r(&t, &tm);

    lua_createtable(L, 0, 6);
    lua_pushinteger(L, tm.tm_year + 1900);
    lua_setfield(L, -2, "year");
    lua_pushinteger(L, tm.tm_mon + 1);
    lua_setfield(L, -2, "month");
    lua_pushinteger(L, tm.tm_mday);
    lua_setfield(L, -2, "day");
    lua_pushinteger(L, tm.tm_hour);
    lua_setfield(L, -2, "hour");
    lua_pushinteger(L, tm.tm_min);
    lua_setfield(L, -2, "min");
    lua_pushinteger(L, tm.tm_sec);
    lua_setfield(L, -2, "sec");
    return 1;
}

/* 堆余量：取 svc_sysinfo 的快照（单位字节）。失败时返回 nil */
static int l_sys_mem(lua_State *L)
{
    svc_sysinfo_t info = { 0 };
    if (svc_sysinfo_get(&info) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)info.heap_internal_free);
    lua_setfield(L, -2, "internal_free");
    lua_pushinteger(L, (lua_Integer)info.heap_internal_min);
    lua_setfield(L, -2, "internal_min");
    lua_pushinteger(L, (lua_Integer)info.heap_psram_free);
    lua_setfield(L, -2, "psram_free");
    return 1;
}

static const luaL_Reg sys_lib[] = {
    { "uptime", l_sys_uptime },
    { "now", l_sys_now },
    { "localtime", l_sys_localtime },
    { "mem", l_sys_mem },
    { NULL, NULL },
};

static int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, sys_lib);
    return 1;
}

/* ------------------------------- 绑定：io ------------------------------- */

static int l_io_gpio_write(lua_State *L)
{
    const lua_Integer gpio = luaL_checkinteger(L, 1);
    const lua_Integer level = luaL_checkinteger(L, 2);
    lua_pushboolean(L, svc_io_gpio_write((uint8_t)gpio, (uint8_t)level) == ESP_OK);
    return 1;
}

static int l_io_gpio_read(lua_State *L)
{
    lua_pushinteger(L, svc_io_gpio_read((uint8_t)luaL_checkinteger(L, 1)));
    return 1;
}

static int l_io_pwm_set(lua_State *L)
{
    const lua_Integer gpio = luaL_checkinteger(L, 1);
    const lua_Integer freq = luaL_checkinteger(L, 2);
    const lua_Integer duty = luaL_checkinteger(L, 3);
    lua_pushboolean(L, svc_io_pwm_set((uint8_t)gpio, (uint32_t)freq, (uint8_t)duty) == ESP_OK);
    return 1;
}

static int l_io_pwm_stop(lua_State *L)
{
    lua_pushboolean(L, svc_io_pwm_stop((uint8_t)luaL_checkinteger(L, 1)) == ESP_OK);
    return 1;
}

static int l_io_adc_read(lua_State *L)
{
    int mv = 0;
    if (svc_io_adc_read((uint8_t)luaL_checkinteger(L, 1), &mv) != ESP_OK) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, mv);
    }
    return 1;
}

static int l_io_i2c_write(lua_State *L)
{
    const lua_Integer addr = luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_pushboolean(L, svc_io_i2c_write((uint8_t)addr, (const uint8_t *)data, len) == ESP_OK);
    return 1;
}

static int l_io_i2c_read(lua_State *L)
{
    const lua_Integer addr = luaL_checkinteger(L, 1);
    const lua_Integer len = luaL_checkinteger(L, 2);
    if (len <= 0 || len > SCRIPT_I2C_READ_MAX) {
        return luaL_error(L, "len must be 1..%d", SCRIPT_I2C_READ_MAX);
    }

    uint8_t buf[SCRIPT_I2C_READ_MAX];
    if (svc_io_i2c_read((uint8_t)addr, buf, (size_t)len) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, (size_t)len);
    return 1;
}

/* UART 占用外扩口 GPIO10 / GPIO11，与 PWM 互斥（服务侧会拒绝占用的引脚）。
 * data_bits 5-8、parity 0=无 1=偶 2=奇、stop_bits 1/2，取值非法由服务返回错误。 */
static int l_io_uart_config(lua_State *L)
{
    const lua_Integer baud = luaL_checkinteger(L, 1);
    const lua_Integer data_bits = luaL_optinteger(L, 2, 8);
    const lua_Integer parity = luaL_optinteger(L, 3, 0);
    const lua_Integer stop_bits = luaL_optinteger(L, 4, 1);

    if (baud <= 0 || data_bits < 5 || data_bits > 8 ||
        parity < 0 || parity > 2 || stop_bits < 1 || stop_bits > 2) {
        lua_pushboolean(L, false);
        return 1;
    }

    lua_pushboolean(L, svc_io_uart_config((uint32_t)baud, (uint8_t)data_bits,
                                          (uint8_t)parity, (uint8_t)stop_bits) == ESP_OK);
    return 1;
}

static int l_io_uart_write(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);

    lua_Integer timeout = SCRIPT_UART_TIMEOUT_DEFAULT;
    if (lua_gettop(L) >= 2) timeout = luaL_checkinteger(L, 2);
    if (timeout < 0) timeout = 0;           /* 0 = 无限等待，与服务侧一致 */

    lua_pushboolean(L, svc_io_uart_write((const uint8_t *)data, len, (uint32_t)timeout) == ESP_OK);
    return 1;
}

/* 读字节：返回字符串；出错或超时（读到 0 字节）返回 nil。len 上限 256 防大缓冲 */
static int l_io_uart_read(lua_State *L)
{
    const lua_Integer len = luaL_checkinteger(L, 1);
    if (len <= 0 || len > SCRIPT_UART_READ_MAX) {
        return luaL_error(L, "len must be 1..%d", SCRIPT_UART_READ_MAX);
    }

    lua_Integer timeout = SCRIPT_UART_TIMEOUT_DEFAULT;
    if (lua_gettop(L) >= 2) timeout = luaL_checkinteger(L, 2);
    if (timeout < 0) timeout = 0;

    uint8_t buf[SCRIPT_UART_READ_MAX];
    size_t got = 0;
    if (svc_io_uart_read(buf, (size_t)len, &got, (uint32_t)timeout) != ESP_OK || got == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, got);
    return 1;
}

static const luaL_Reg io_lib[] = {
    { "gpio_write", l_io_gpio_write },
    { "gpio_read", l_io_gpio_read },
    { "pwm_set", l_io_pwm_set },
    { "pwm_stop", l_io_pwm_stop },
    { "adc_read", l_io_adc_read },
    { "i2c_write", l_io_i2c_write },
    { "i2c_read", l_io_i2c_read },
    { "uart_config", l_io_uart_config },
    { "uart_write", l_io_uart_write },
    { "uart_read", l_io_uart_read },
    { NULL, NULL },
};

/* 注意：本函数不能叫 luaopen_io —— lualib.h 已为 Lua 标准 io 库声明了同名外部符号，
 * 静态定义会与之冲突。这里用 luaopen_io_hw 表示"硬件 io 模块"。 */
static int luaopen_io_hw(lua_State *L)
{
    luaL_newlib(L, io_lib);
    return 1;
}

/* ------------------------------ 绑定：file ------------------------------ */

static int l_file_read(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    void *buf = NULL;
    size_t len = 0;
    if (svc_storage_read(path, &buf, &len) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, len);
    free(buf);
    return 1;
}

static int l_file_write(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_pushboolean(L, svc_storage_write(path, data, len) == ESP_OK);
    return 1;
}

static int l_file_remove(lua_State *L)
{
    lua_pushboolean(L, svc_storage_remove(luaL_checkstring(L, 1)) == ESP_OK);
    return 1;
}

static int l_file_exists(lua_State *L)
{
    lua_pushboolean(L, svc_storage_exists(luaL_checkstring(L, 1), NULL) == ESP_OK);
    return 1;
}

static int l_file_list(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    int n = 0;
    svc_storage_entry_t *e = NULL;
    while ((e = svc_storage_iter_next(it)) != NULL) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, e->name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)e->size);
        lua_setfield(L, -2, "size");
        lua_pushboolean(L, e->is_dir);
        lua_setfield(L, -2, "dir");
        lua_rawseti(L, -2, ++n);
    }
    svc_storage_iter_end(it);
    return 1;
}

static const luaL_Reg file_lib[] = {
    { "read", l_file_read },
    { "write", l_file_write },
    { "remove", l_file_remove },
    { "exists", l_file_exists },
    { "list", l_file_list },
    { NULL, NULL },
};

static int luaopen_file(lua_State *L)
{
    luaL_newlib(L, file_lib);
    return 1;
}

/* ------------------------------ 绑定：audio ------------------------------ */

static int l_audio_play(lua_State *L)
{
    const char *uri = luaL_checkstring(L, 1);

    svc_audio_source_t src = { 0 };
    /* http:// 或 https:// 开头走链接边下边播，其余按本地文件路径 */
    src.type = (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0)
                   ? SVC_AUDIO_SRC_URL
                   : SVC_AUDIO_SRC_FILE;
    src.uri = uri;

    if (lua_gettop(L) >= 2) {
        src.loop = lua_toboolean(L, 2);
    }
    lua_pushboolean(L, svc_audio_play(&src) == ESP_OK);
    return 1;
}

static int l_audio_stop(lua_State *L)
{
    (void)L;
    svc_audio_stop();
    return 0;
}

/* 外部 PCM 流：stream_start(rate[, channels]) → write(pcm) × N → stream_stop() */
static int l_audio_stream_start(lua_State *L)
{
    periph_audio_format_t fmt = {
        .sample_rate = PERIPH_AUDIO_SR_16K, .bit_width = 16, .channels = 2,
    };
    if (lua_gettop(L) >= 1) {
        fmt.sample_rate = (periph_audio_sample_rate_t)luaL_checkinteger(L, 1);
    }
    if (lua_gettop(L) >= 2) {
        lua_Integer ch = luaL_checkinteger(L, 2);
        fmt.channels = (uint8_t)((ch == 1) ? 1 : 2);
    }
    lua_pushboolean(L, svc_audio_stream_start(&fmt) == ESP_OK);
    return 1;
}

/* Lua 字符串按字节传入，内容就是 16-bit PCM */
static int l_audio_write(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    if (len == 0) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, svc_audio_write((const uint8_t *)data, len, 0) == ESP_OK);
    return 1;
}

static int l_audio_stream_stop(lua_State *L)
{
    (void)L;
    lua_pushboolean(L, svc_audio_stream_stop() == ESP_OK);
    return 1;
}

static int l_audio_get_volume(lua_State *L)
{
    lua_pushinteger(L, svc_audio_get_volume());
    return 1;
}

static int l_audio_set_volume(lua_State *L)
{
    lua_pushboolean(L, svc_audio_set_volume((uint8_t)luaL_checkinteger(L, 1)) == ESP_OK);
    return 1;
}

static int l_audio_set_mute(lua_State *L)
{
    lua_pushboolean(L, svc_audio_set_mute(lua_toboolean(L, 1)) == ESP_OK);
    return 1;
}

/* 录音：svc_audio 的录音任务跑在音频任务里，这里只投递请求 / 请求收尾。
 * max_seconds 不写默认 60 s（服务侧 0 也视为 60 s），上限 1 小时兜底。 */
static int l_audio_record_start(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_Integer sec = SCRIPT_RECORD_SEC_DEFAULT;
    if (lua_gettop(L) >= 2) sec = luaL_checkinteger(L, 2);

    if (sec <= 0) sec = SCRIPT_RECORD_SEC_DEFAULT;
    if (sec > SCRIPT_RECORD_SEC_MAX) sec = SCRIPT_RECORD_SEC_MAX;

    lua_pushboolean(L, svc_audio_record_start(path, (uint32_t)sec) == ESP_OK);
    return 1;
}

static int l_audio_record_stop(lua_State *L)
{
    (void)L;
    /* 不在录音时服务返回 ESP_ERR_INVALID_STATE，这里统一反映成 false */
    lua_pushboolean(L, svc_audio_record_stop() == ESP_OK);
    return 1;
}

/* 提示音：tone(freq_hz[, ms])，默认 200 ms。异步播放（不阻塞脚本任务），
 * 会打断当前播放；频率 / 时长为非法值时返回 false，不调用服务。 */
static int l_audio_tone(lua_State *L)
{
    const lua_Integer freq = luaL_checkinteger(L, 1);
    const lua_Integer ms = luaL_optinteger(L, 2, 200);

    if (freq <= 0 || freq > 20000 || ms <= 0) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, svc_audio_play_tone_async((uint16_t)freq, (uint32_t)ms) == ESP_OK);
    return 1;
}

static const luaL_Reg audio_lib[] = {
    { "play", l_audio_play },
    { "stop", l_audio_stop },
    { "tone", l_audio_tone },
    { "stream_start", l_audio_stream_start },
    { "write", l_audio_write },
    { "stream_stop", l_audio_stream_stop },
    { "record_start", l_audio_record_start },
    { "record_stop", l_audio_record_stop },
    { "get_volume", l_audio_get_volume },
    { "set_volume", l_audio_set_volume },
    { "set_mute", l_audio_set_mute },
    { NULL, NULL },
};

static int luaopen_audio(lua_State *L)
{
    luaL_newlib(L, audio_lib);
    return 1;
}

/* ------------------------------- 绑定：net ------------------------------- */

/* 阻塞调用（最长 15 s）：脚本任务不纳入看门狗，靠指令数钩子兜死循环 */
static int l_net_http_get(lua_State *L)
{
    const char *url = luaL_checkstring(L, 1);
    lua_Integer timeout = 15000;
    if (lua_gettop(L) >= 2) {
        timeout = luaL_checkinteger(L, 2);
    }

    char *buf = malloc(SCRIPT_HTTP_BUF);
    if (buf == NULL) return luaL_error(L, "out of memory");

    const esp_err_t err = svc_http_get(url, buf, SCRIPT_HTTP_BUF, (uint32_t)timeout);
    if (err != ESP_OK) {
        free(buf);
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    free(buf);
    return 1;
}

static const luaL_Reg net_lib[] = {
    { "http_get", l_net_http_get },
    { NULL, NULL },
};

static int luaopen_net(lua_State *L)
{
    luaL_newlib(L, net_lib);
    return 1;
}

/* ------------------------------ 绑定：timer ------------------------------ */

static void timers_reset(void)
{
    for (int i = 0; i < SCRIPT_TIMER_MAX; i++) {
        s_timers[i].ref = LUA_NOREF;
        s_timers[i].period_ms = 0;
        s_timers[i].next_us = 0;
    }
}

static int l_timer_every(lua_State *L)
{
    const lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms <= 0) return luaL_error(L, "period must be > 0");

    for (int i = 0; i < SCRIPT_TIMER_MAX; i++) {
        if (s_timers[i].ref == LUA_NOREF) {
            lua_pushvalue(L, 2);
            s_timers[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            s_timers[i].period_ms = (uint32_t)ms;
            s_timers[i].next_us = esp_timer_get_time() + (int64_t)ms * 1000;
            lua_pushinteger(L, i + 1);
            return 1;
        }
    }
    return luaL_error(L, "too many timers (max %d)", SCRIPT_TIMER_MAX);
}

static const luaL_Reg timer_lib[] = {
    { "every", l_timer_every },
    { NULL, NULL },
};

static int luaopen_timer(lua_State *L)
{
    luaL_newlib(L, timer_lib);
    return 1;
}

/* ------------------------------- 运行时 ------------------------------- */

static void run_due_timers(void)
{
    const int64_t now = esp_timer_get_time();

    for (int i = 0; i < SCRIPT_TIMER_MAX; i++) {
        if (s_timers[i].ref == LUA_NOREF) continue;
        if (now < s_timers[i].next_us) continue;

        s_timers[i].next_us = now + (int64_t)s_timers[i].period_ms * 1000;

        lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_timers[i].ref);
        if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "timer callback error: %s", lua_tostring(s_L, -1));
            lua_pop(s_L, 1);
        }
    }
}

/* 只打开纯计算库 */
static void open_safe_libs(lua_State *L)
{
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
}

/* --------------------------- 能力裁剪（SCR-006） ---------------------------
 *
 * 脚本头部可以写一行注释 `-- @perm io,file,net` 声明要用的模块；写了声明就
 * 只注册列出的模块，未声明的模块在脚本里是 nil、调用即 Lua 报错。不写声明的
 * 脚本（含内置示例）保持"全部模块可用"。
 *
 * `ui` / `sys` / `timer` 标记为基础能力，无论声明如何都注册：脚本的界面、时间
 * 与定时器是它自己"能跑起来并报错"的最小前提，否则连 `ui.toast("脚本出错")`
 * 都发不出来。
 */

#define SCRIPT_MODULE_BASE  0x01

typedef struct {
    const char *name;
    lua_CFunction open;
    uint8_t flags;
} script_module_t;

/* 模块名就是 Lua 全局表名，也是 @perm 里写的名字 */
static const script_module_t SCRIPT_MODULES[] = {
    { "ui",       luaopen_ui,       SCRIPT_MODULE_BASE },
    { "sys",      luaopen_sys,      SCRIPT_MODULE_BASE },
    { "timer",    luaopen_timer,    SCRIPT_MODULE_BASE },
    { "input",    luaopen_input,    0 },
    { "event",    luaopen_event,    0 },
    { "io",       luaopen_io_hw,    0 },
    { "file",     luaopen_file,     0 },
    { "audio",    luaopen_audio,    0 },
    { "net",      luaopen_net,      0 },
    { "mqtt",     luaopen_mqtt,     0 },
    { "ws",       luaopen_ws,       0 },
    { "bt",       luaopen_bt,       0 },
    { "camera",   luaopen_camera,   0 },
    { "imu",      luaopen_imu,      0 },
    { "settings", luaopen_settings, 0 },
};
#define SCRIPT_MODULE_COUNT (sizeof(SCRIPT_MODULES) / sizeof(SCRIPT_MODULES[0]))

typedef struct {
    bool declared;                          /* 脚本头部是否写了 @perm */
    bool allow[SCRIPT_MODULE_COUNT];
} script_perm_t;

/* 按逗号 / 空白切分模块名；认不出的名字告警并忽略 */
static void parse_perm_list(const char *list, script_perm_t *perm)
{
    char name[24];
    size_t n = 0;

    for (const char *p = list; ; p++) {
        const char c = *p;
        if (c == ',' || c == ' ' || c == '\t' || c == '\0') {
            if (n > 0) {
                name[n] = '\0';

                bool known = false;
                for (size_t i = 0; i < SCRIPT_MODULE_COUNT; i++) {
                    if (strcmp(SCRIPT_MODULES[i].name, name) == 0) {
                        perm->allow[i] = true;
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    ESP_LOGW(TAG, "unknown module in @perm: %s", name);
                }
                n = 0;
            }
            if (c == '\0') break;
            continue;
        }
        if (n + 1 < sizeof(name)) {
            name[n++] = c;
        }
    }
}

/* 解析范围与 read_meta() 一致：只看文件开头 SCRIPT_META_LINES 行，只认第一处声明。
 * 读不到文件时按"未声明"处理（此时 start_script 也会因为加载失败而报错）。 */
static void read_perm(const char *path, script_perm_t *perm)
{
    memset(perm, 0, sizeof(*perm));

    FILE *fp = fopen(path, "r");
    if (fp == NULL) return;

    char line[160];
    for (int i = 0; i < SCRIPT_META_LINES && fgets(line, sizeof(line), fp) != NULL; i++) {
        const char *p = strstr(line, "@perm");
        if (p == NULL) continue;

        p += strlen("@perm");
        while (*p == ' ' || *p == '\t') p++;

        char list[120];
        snprintf(list, sizeof(list), "%.*s", (int)sizeof(list) - 1, p);
        for (char *q = list; *q != '\0'; q++) {
            if (*q == '\n' || *q == '\r') {
                *q = '\0';
                break;
            }
        }

        parse_perm_list(list, perm);
        perm->declared = true;
        break;
    }
    fclose(fp);
}

static bool perm_allow(const script_perm_t *perm, size_t idx)
{
    if (!perm->declared) return true;                               /* 没写声明 = 全部可用 */
    if (SCRIPT_MODULES[idx].flags & SCRIPT_MODULE_BASE) return true; /* 基础能力始终注册 */
    return perm->allow[idx];
}

static void register_modules(lua_State *L, const script_perm_t *perm)
{
    for (size_t i = 0; i < SCRIPT_MODULE_COUNT; i++) {
        if (!perm_allow(perm, i)) {
            ESP_LOGI(TAG, "module not permitted: %s", SCRIPT_MODULES[i].name);
            continue;                       /* 不注册，脚本里就是 nil */
        }
        luaL_requiref(L, SCRIPT_MODULES[i].name, SCRIPT_MODULES[i].open, 1);
        lua_pop(L, 1);
    }
}

static void toast(const char *msg)
{
    lvgl_port_lock(0);
    fw_ui_toast(msg, 2500);
    lvgl_port_unlock();
}

/* 指令数钩子：一次 Lua 调用跑太多指令就当中止（死循环兜底） */
static void script_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "instruction budget exceeded (%ld)", SCRIPT_INSN_BUDGET);
}

/* 停止 / 启动失败时统一回收：删脚本界面、退订事件、断开 MQTT / WebSocket、关掉
 * Lua 状态。回调队列里可能还留着指向这个状态的函数引用，一并清掉。 */
static void teardown_script(void)
{
    lvgl_port_lock(0);

    /* 先把屏还回去，再删脚本页 —— 顺序不能反：删除活动屏会把 display 的 act_scr
     * 置空，之后任何 lv_screen_load* 都会空指针崩溃（AGENTS 4.12）。
     *
     * 只有脚本页确实在前台时才切：用户可能在脚本运行中按了状态栏返回键，那时前台
     * 已经是别的屏，硬切回 s_prev_screen 反而会抢回前台；此时直接删页即可。
     * fw_window 的 s_active 是裸指针，先 sync 再和 lv_screen_active() 一起核对。 */
    fw_window_sync_active();
    if (s_page != NULL && s_prev_screen != NULL && lv_screen_active() == s_page) {
        /* s_prev_screen 是活的：它只会在 s_page 不在前台时被重记（见 l_ui_page），
         * 而重记与切屏在同一次加锁内完成，中间不会有主题重建把目标屏删掉。 */
        fw_window_switch_to(s_prev_screen, LV_SCR_LOAD_ANIM_NONE, 0);
    }
    s_prev_screen = NULL;

    /* 脚本建的界面对象随页面根一起删掉 */
    if (s_page != NULL) {
        lv_obj_delete(s_page);
        s_page = NULL;
    }
    lvgl_port_unlock();

    /* 先退订：否则事件回调会打到马上要关掉的 Lua 状态上 */
    for (int i = 0; i < SCRIPT_EVENT_MAX; i++) {
        if (s_subs[i].ref != LUA_NOREF) {
            svc_event_bus_unsubscribe(s_subs[i].id, bus_event_cb);
        }
    }
    subs_reset();

    external_reset();               /* 退订 MQTT 主题 + 断开 MQTT / WebSocket */
    flush_callback_queues();        /* 清掉回调队列里属于本状态的引用 */

    if (s_L != NULL) {
        lua_close(s_L);
        s_L = NULL;
    }
    timers_reset();
    flush_callback_queues();        /* 关闭过程中可能又有回调投递进来，再清一次 */
}

static void start_script(void)
{
    /* 上一次异常退出可能留下没取走的事件，先丢掉 */
    flush_callback_queues();

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        ESP_LOGE(TAG, "luaL_newstate failed (out of memory)");
        toast("内存不足，脚本未启动");
        return;
    }

    s_L = L;
    timers_reset();
    open_safe_libs(L);

    /* 能力裁剪先于注册：@perm 只在文件头部若干行内解析（见 read_perm） */
    script_perm_t perm;
    read_perm(s_pending_path, &perm);
    register_modules(L, &perm);
    if (perm.declared) {
        ESP_LOGI(TAG, "perm declared: only listed modules registered");
    }

    lua_sethook(L, script_hook, LUA_MASKCOUNT, SCRIPT_INSN_BUDGET);

    if (luaL_loadfile(L, s_pending_path) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "script error: %s", lua_tostring(L, -1));
        toast("脚本出错，见日志");
        teardown_script();          /* 脚本可能已经建了界面 / 订阅，按停止处理 */
        s_current[0] = '\0';
        return;
    }

    s_running = true;
    toast("脚本已启动");
    svc_event_bus_publish(SVC_EVENT_SCRIPT_STARTED, NULL, 0);
    ESP_LOGI(TAG, "running: %s", s_pending_path);
}

static void stop_script(void)
{
    teardown_script();

    s_running = false;
    s_current[0] = '\0';

    toast("脚本已停止");
    svc_event_bus_publish(SVC_EVENT_SCRIPT_STOPPED, NULL, 0);
    ESP_LOGI(TAG, "stopped");
}

static void script_task(void *arg)
{
    (void)arg;

    /* 不纳入 Task WDT：net.http_get 这类绑定会阻塞数秒，纳入会被误判成卡死；
     * 脚本死循环改由 Lua 指令数钩子中止（见 script_hook）。 */

    while (true) {
        script_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(SCRIPT_TICK_MS)) == pdTRUE) {
            if (cmd == CMD_RUN && !s_running) {
                start_script();
            } else if (cmd == CMD_STOP && s_running) {
                stop_script();
            }
        }

        if (s_running && s_L != NULL) {
            drain_widget_events();
            drain_bus_events();
            drain_msg_events();
            run_due_timers();
        }
    }
}

/* --------------------- 内置示例脚本（首次启动释放） --------------------- */

/* 开头的 -- @name / -- @desc 会被 fw_script_scan() 解析，脚本管理器据此显示名称与说明 */

static const char SAMPLE_CLOCK_LUA[] =
    "-- @name 时钟\n"
    "-- @desc 每秒刷新时间，点击屏幕切换 12 / 24 小时制\n"
    "\n"
    "local hour24 = true\n"
    "local page = ui.page()\n"
    "local label = ui.label(page, \"00:00:00\")\n"
    "\n"
    "local function on_tick()\n"
    "    local t = sys.localtime()\n"
    "    local h = t.hour\n"
    "    if not hour24 then\n"
    "        h = h % 12\n"
    "        if h == 0 then h = 12 end\n"
    "    end\n"
    "    label:set_text(string.format(\"%02d:%02d:%02d\", h, t.min, t.sec))\n"
    "end\n"
    "\n"
    "input.on_click(function()\n"
    "    hour24 = not hour24\n"
    "    ui.toast(hour24 and \"24 小时制\" or \"12 小时制\")\n"
    "end)\n"
    "\n"
    "timer.every(1000, on_tick)\n"
    "on_tick()\n";

static const char SAMPLE_GPIO_LUA[] =
    "-- @name 外扩 IO\n"
    "-- @desc 每秒翻转 GPIO10，并读回 GPIO11 的电平\n"
    "\n"
    "local page = ui.page()\n"
    "local label = ui.label(page, \"GPIO10 = 0\")\n"
    "\n"
    "local level = 0\n"
    "\n"
    "local function on_tick()\n"
    "    level = 1 - level\n"
    "    io.gpio_write(10, level)\n"
    "    local back = io.gpio_read(11)\n"
    "    label:set_text(string.format(\"GPIO10 = %d  GPIO11 = %d\", level, back))\n"
    "end\n"
    "\n"
    "timer.every(1000, on_tick)\n"
    "on_tick()\n";

/* 把内置示例释放到脚本目录：同名不覆盖。写文件要 TF 卡，/sdcard 没挂载时只告警，
 * 不影响启动；目录不存在时先建（已存在会失败，忽略）。 */
static void release_samples(void)
{
    static const struct {
        const char *name;
        const char *src;
    } samples[] = {
        { "clock.lua", SAMPLE_CLOCK_LUA },
        { "gpio.lua",  SAMPLE_GPIO_LUA },
    };

    svc_storage_mkdir(FW_SCRIPT_DIR);

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        char path[FW_SCRIPT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", FW_SCRIPT_DIR, samples[i].name);

        if (svc_storage_exists(path, NULL) == ESP_OK) continue;     /* 同名不覆盖 */

        if (svc_storage_write(path, samples[i].src, strlen(samples[i].src)) != ESP_OK) {
            ESP_LOGW(TAG, "release sample failed (no TF card?): %s", path);
        } else {
            ESP_LOGI(TAG, "sample released: %s", path);
        }
    }
}

esp_err_t fw_script_init(void)
{
    if (s_task != NULL) return ESP_OK;

    timers_reset();

    s_queue = xQueueCreate(4, sizeof(script_cmd_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    s_cb_queue = xQueueCreate(SCRIPT_CB_QUEUE_LEN, sizeof(int));
    if (s_cb_queue == NULL) return ESP_ERR_NO_MEM;

    s_bus_queue = xQueueCreate(SCRIPT_EVENT_MAX, sizeof(script_bus_evt_t));
    if (s_bus_queue == NULL) return ESP_ERR_NO_MEM;

    s_msg_queue = xQueueCreate(SCRIPT_MSG_QUEUE_LEN, sizeof(script_msg_evt_t));
    if (s_msg_queue == NULL) return ESP_ERR_NO_MEM;

    subs_reset();

    if (xTaskCreatePinnedToCore(script_task, "script_task", SCRIPT_TASK_STACK, NULL,
                                SCRIPT_TASK_PRIO, &s_task, 0) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    release_samples();

    ESP_LOGI(TAG, "initialized (scripts in %s)", FW_SCRIPT_DIR);
    return ESP_OK;
}

/* ---------------------------- 扫描 / 元信息 ---------------------------- */

static void name_of(const char *path, char *out, size_t len)
{
    const char *base = strrchr(path, '/');
    snprintf(out, len, "%s", base ? base + 1 : path);

    char *dot = strrchr(out, '.');
    if (dot != NULL) *dot = '\0';
}

/* 取 "-- @name xxx" / "-- @desc xxx" 里 tag 后面的内容，去掉首尾空白 */
static void meta_value(const char *line, const char *tag, char *out, size_t len)
{
    const char *p = strstr(line, tag);
    if (p == NULL) return;

    p += strlen(tag);
    while (*p == ' ' || *p == '\t') p++;

    size_t n = 0;
    while (p[n] != '\0' && p[n] != '\n' && p[n] != '\r' && n + 1 < len) {
        out[n] = p[n];
        n++;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) n--;
    out[n] = '\0';
}

static void read_meta(fw_script_info_t *info)
{
    name_of(info->path, info->name, sizeof(info->name));
    info->desc[0] = '\0';

    FILE *fp = fopen(info->path, "r");
    if (fp == NULL) return;

    char line[160];
    for (int i = 0; i < SCRIPT_META_LINES && fgets(line, sizeof(line), fp) != NULL; i++) {
        if (strstr(line, "@name") != NULL) {
            meta_value(line, "@name", info->name, sizeof(info->name));
        } else if (strstr(line, "@desc") != NULL) {
            meta_value(line, "@desc", info->desc, sizeof(info->desc));
        }
    }
    fclose(fp);
}

esp_err_t fw_script_scan(fw_script_info_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL) return ESP_ERR_INVALID_ARG;

    *count = 0;

    svc_storage_iter_t it = NULL;
    esp_err_t err = svc_storage_iter_start(FW_SCRIPT_DIR, &it);
    if (err != ESP_OK) return err;

    svc_storage_entry_t *e = NULL;
    while (*count < max && (e = svc_storage_iter_next(it)) != NULL) {
        if (e->is_dir) continue;

        const size_t n = strlen(e->name);
        if (n < 4 || strcmp(e->name + n - 4, ".lua") != 0) continue;

        fw_script_info_t *info = &out[*count];
        snprintf(info->path, sizeof(info->path), "%s/%.140s", FW_SCRIPT_DIR, e->name);
        read_meta(info);
        (*count)++;
    }

    svc_storage_iter_end(it);
    return ESP_OK;
}

/* -------------------------------- 运行 / 停止 -------------------------------- */

esp_err_t fw_script_run(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (s_running) return ESP_ERR_INVALID_STATE;

    snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
    name_of(s_pending_path, s_current, sizeof(s_current));

    const script_cmd_t cmd = CMD_RUN;
    return (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

esp_err_t fw_script_stop(void)
{
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_running) return ESP_OK;

    const script_cmd_t cmd = CMD_STOP;
    return (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

bool fw_script_is_running(void)
{
    return s_running;
}

const char *fw_script_current(void)
{
    return s_running ? s_current : NULL;
}
