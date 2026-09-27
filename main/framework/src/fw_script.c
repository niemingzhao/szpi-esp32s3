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
 * 能力绑定（脚本侧模块名 = Lua 全局表名 = -- @perm 里写的名字；下面按模块列出，
 * 板子上随固件释放的「脚本接口参考.txt」是同一份内容的可查版本）：
 *   - ui   页面与控件：page / label / row / image / progress / canvas / input /
 *            dialog / list / item / button / box / grid / toast；控件方法
 *            set_text / set_value / set_src / set / get_text / set_size /
 *            set_color / set_text_color / set_radius，画布另有 fill / rect /
 *            line / circle（ui.page 建好页面就切屏，停止 / 出错时切回脚本运行前的屏）
 *   - sys  时间与内存：uptime / now / localtime / mem / exit（自己结束，等价于
 *            点「停止」）；系统级设置（亮度 / 主题 / 时区）不开放给脚本
 *   - timer every / after / cancel（后两个是一次性定时器与取消）
 *   - input on_click（触摸点击，回调收到 {x, y, pressed}）；BOOT 键用 event.subscribe("key")
 *   - event subscribe / publish（固定事件名见 EVENT_MAP，另有 user. 自定义事件名）
 *   - io   外扩口：gpio_write / gpio_read / pwm_set / pwm_stop / adc_read /
 *            i2c_write / i2c_read / i2c_write_reg / i2c_read_reg / uart_config /
 *            uart_write / uart_read / can_config / can_stop / can_send / can_receive
 *            （一律经 svc_io，驱动不进 Lua）
 *   - file read / write / append / size / remove / exists / list
 *   - audio play / stop / tone / record_start / record_stop / get_volume /
 *            set_volume / set_mute / stream_start / write / stream_stop（外部 PCM 流）
 *   - net  http_get / http_post（https 走内置根证书；不跟随重定向）
 *   - mqtt connect / publish / subscribe / disconnect / is_connected
 *   - ws   connect(url, fn)（fn 收消息）/ send / is_connected / close
 *   - bt   从机：adv / is_connected / state_name / scan_start / scan_stop /
 *            scan_results / notify；HID：hid_key / hid_mouse / hid_consumer（要主机
 *            配对完成才发得出去，未就绪返回 false）；中心角色：gatt_connect / gatt_disconnect /
 *            gatt_state / gatt_discover / gatt_services / gatt_chars / gatt_read /
 *            gatt_write / gatt_subscribe / gatt_on_notify
 *   - camera open([jpeg]) / close / is_open / capture(path)（RGB565 帧在设备侧转成
 *            24 位 BMP 落盘，和相机 App 同一套；open(true) 的 JPEG 模式按码流原样存）
 *   - imu  read / is_moving / orientation
 *   - settings get / set / get_int / set_int（写 NVS 的 script 命名空间）
 *   - util base64_encode / base64_decode / hex_encode / hex_decode
 *   - json decode / encode（cJSON）
 *   - web  局域网网页：get / post 注册路由（挂在 Web 控制台的 /s 前缀下、只在脚本
 *            运行期间有效）、url / ready；回调收到 {method, path, query（表）, query_string,
 *            body}，返回 (content_type, body) 或 (status, content_type, body)
 *
 * 能力裁剪：脚本头部可以写 -- @perm io,file,net 声明要用的模块，
 * 写了声明就只注册列出的模块（未声明的模块在脚本里是 nil，调用即 Lua 报错）；
 * 不写声明表示全部可用，兼容内置示例脚本。ui / sys / timer 是基础能力，
 * 始终注册 —— 否则脚本连报错提示都发不出来。解析见 read_perm()。
 * 各项数量上限（定时器 / 订阅 / 控件 / 画布、单次调用的指令数、消息与响应体大小）见
 * 上面那批 SCRIPT_* 宏。
 *
 * 脚本配置：settings.* 统一写进 NVS 的 "script" 命名空间，key 原样使用
 * （完整标识是 ns="script" + key，与 sys / net / audio / theme / app_* 隔离）。
 * NVS 的 key 上限 15 个字符，超长时 set 返回 false、get 取到默认值。
 *
 * 每次启动都会把内置示例脚本释放到脚本目录（同名覆盖，便于随固件更新；
 * 无 TF 卡时仅告警）。
 *
 * 脚本的输出：print 被换成自己的实现，一行同时进脚本日志环与串口日志（标签 script）。
 * 没有界面的脚本（不调 ui.page()）启动后会自动获得一个「运行日志」页显示这些输出，
 * 于是"有无界面"的运行逻辑一致 —— 脚本总有自己的一页，离开它（返回键 / 双击 / 主页键 /
 * 换主题）就停脚本，判断见 fw_script_owns_screen()。脚本也可以自己调 sys.exit() 退出。
 */

#include "fw_common.h"
#include "svc_common.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "fw.script";

/* 内置示例脚本：assets/scripts/ 下的文件在构建期用 EMBED_FILES 嵌进来，符号名按 IDF 规则只取
 * 文件名（counter.lua → _binary_counter_lua_start）。释放到卡上的名字见 release_samples()。 */
extern const char counter_lua_start[] asm("_binary_counter_lua_start");
extern const char counter_lua_end[] asm("_binary_counter_lua_end");
extern const char clock_lua_start[] asm("_binary_clock_lua_start");
extern const char clock_lua_end[] asm("_binary_clock_lua_end");
extern const char snake_lua_start[] asm("_binary_snake_lua_start");
extern const char snake_lua_end[] asm("_binary_snake_lua_end");
extern const char io_lua_start[] asm("_binary_io_lua_start");
extern const char io_lua_end[] asm("_binary_io_lua_end");
extern const char gomoku_lua_start[] asm("_binary_gomoku_lua_start");
extern const char gomoku_lua_end[] asm("_binary_gomoku_lua_end");
/* 资源文件名是 ASCII，释放到卡上叫「脚本接口参考.txt」（脚本管理只列 .lua，不会混进列表） */
extern const char api_reference_txt_start[] asm("_binary_api_reference_txt_start");
extern const char api_reference_txt_end[] asm("_binary_api_reference_txt_end");

#define SCRIPT_TASK_STACK   8192
#define SCRIPT_TASK_PRIO    4
#define SCRIPT_TICK_MS      20
#define SCRIPT_TIMER_MAX    16
#define SCRIPT_META_LINES   20
/* 单次 Lua 调用的指令预算：防止脚本死循环把系统拖住（超了当脚本错误处理） */
#define SCRIPT_INSN_BUDGET  20000000L
#define SCRIPT_HTTP_BUF     8192
#define SCRIPT_I2C_READ_MAX 64
#define SCRIPT_UART_READ_MAX 256
#define SCRIPT_UART_TIMEOUT_DEFAULT 1000
#define SCRIPT_CAN_TIMEOUT_MS       100
#define SCRIPT_BT_SCAN_SEC_DEFAULT  5
#define SCRIPT_RECORD_SEC_DEFAULT   60
#define SCRIPT_RECORD_SEC_MAX       3600

typedef enum { CMD_RUN = 1, CMD_STOP } script_cmd_t;

typedef struct {
    int ref;                    /* LUA_REGISTRYINDEX 里的函数引用；LUA_NOREF = 空闲 */
    uint32_t period_ms;
    int64_t next_us;
    bool one_shot;              /* timer.after：只跑一次 */
} script_timer_t;

static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static lua_State *s_L = NULL;
static bool s_running = false;
static volatile bool s_starting = false;    /* 已投递 CMD_RUN、任务还没开始跑 */
static char s_current[FW_SCRIPT_NAME_MAX] = { 0 };
static char s_pending_path[FW_SCRIPT_PATH_MAX] = { 0 };
static char s_last_error[FW_SCRIPT_ERR_MAX] = { 0 };   /* 最近一次脚本错误，见 fw_script_last_error() */
static script_timer_t s_timers[SCRIPT_TIMER_MAX];

/* 界面提示（自动加 LVGL 锁），下面几处都用得到 */
static void toast(const char *msg);
/* 脚本出错统一走它：记错误 + 串口 + 提示 + 通知（见实现） */
static void script_error(const char *msg, bool notify);
/* 统一调用脚本回调：真错误交给 script_error，sys.exit 的展开静默吞掉 */
static void script_pcall(lua_State *L, int nargs, int nres);

/* sys.exit() 的特殊错误值（拿它的地址做标记），见 l_sys_exit */
static const char s_exit_marker;

/* ------------------------------- 绑定：ui ------------------------------- */

#define SCRIPT_WIDGET_MT    "szpi.widget"
#define SCRIPT_CB_QUEUE_LEN 16

static lv_obj_t *s_page = NULL;             /* 脚本的页面根，停止时整体删除 */
static lv_obj_t *s_prev_screen = NULL;      /* 脚本界面之前正在显示的屏，停止时还回去 */
static QueueHandle_t s_cb_queue = NULL;     /* LVGL 事件 -> 脚本任务 */

/* 「运行日志」页（无界面脚本的落地页）与它的刷新定时器，见 script_log_page_open */
static lv_obj_t *s_log_box = NULL;
static lv_obj_t *s_log_lb = NULL;
static lv_timer_t *s_log_timer = NULL;
static char *s_log_buf = NULL;              /* 日志环尾部（刷新用，常驻） */
static char *s_log_prev = NULL;             /* 上一帧显示的文本，没变就不重排 */
static void script_log_page_open(void);
static void script_log_page_close(void);

/* 脚本输入框的软键盘（挂在脚本页根上，一个页面共用一个） */
static lv_obj_t *s_kb = NULL;
static lv_obj_t *s_kb_for = NULL;           /* 键盘当前服务的输入框 */

/* 本脚本创建过的控件登记表：用来判断一个 Lua 控件句柄还有没有效。
 *
 * 不能用 lv_obj_is_in_widget_tree()：LVGL 9.6 里它只是 lv_obj_is_in_widget_tree 的别名，
 * 会顺着 obj->parent 往上解引用，对已释放的对象用会直接崩。
 * 改成自己登记：重建页面（ui.page）、停止脚本时整表清空，之后的旧句柄一律按
 * "无效"处理 —— set_* 静默跳过，建控件（label / row / image / progress）返回 nil
 * 让脚本自己报错，都不会解引用悬空指针。登记表只在脚本任务里读写，不需要加锁。 */
#define SCRIPT_WIDGET_MAX 256
static lv_obj_t *s_widgets[SCRIPT_WIDGET_MAX];
static size_t s_widget_count = 0;
static bool s_widget_full_warned = false;

/* 画布（ui.canvas）：脚本自己画的 RGB565 位图，交给 LVGL 的是"整幅已解码 RAM 图"
 *（与图库 App 手动解 BMP 同一套 lv_image_dsc_t 用法），所以能直接改像素重绘。
 * 缓冲区在换页 / 停止脚本时统一释放（见 canvases_reset）。 */
#define SCRIPT_CANVAS_MAX      8
#define SCRIPT_CANVAS_MAX_W    320
#define SCRIPT_CANVAS_MAX_H    212

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *px;
} script_canvas_t;

static script_canvas_t *s_canvases[SCRIPT_CANVAS_MAX];
static size_t s_canvas_count = 0;

static void canvases_reset(void)
{
    for (size_t i = 0; i < s_canvas_count; i++) {
        if (s_canvases[i] != NULL) {
            free(s_canvases[i]->px);
            free(s_canvases[i]);
            s_canvases[i] = NULL;
        }
    }
    s_canvas_count = 0;
}

static void widgets_reset(void)
{
    s_widget_count = 0;
    s_widget_full_warned = false;
    canvases_reset();

    /* 软键盘是脚本页的子对象，页面删了就一起没了：句柄必须跟着清 */
    s_kb = NULL;
    s_kb_for = NULL;
}

static bool widget_alive(lv_obj_t *obj)
{
    if (obj == NULL) return false;

    for (size_t i = 0; i < s_widget_count; i++) {
        if (s_widgets[i] == obj) return true;
    }
    return false;
}

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

    if (s_widget_count < SCRIPT_WIDGET_MAX) {
        s_widgets[s_widget_count++] = obj;
    } else if (!s_widget_full_warned) {
        s_widget_full_warned = true;
        ESP_LOGW(TAG, "widget registry full (%d), later handles will look invalid",
                 SCRIPT_WIDGET_MAX);
    }
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
 * 切之前先记下"脚本界面之前"正在显示的屏（正常路径就是脚本管理 Scripts 的列表根屏），
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
     *   - 第一次建页：前台是启动脚本的那个 App（正常为脚本管理 Scripts）。
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
        /* 先关掉「运行日志」页的刷新定时器，再切屏 / 删旧页：它的标签就挂在旧页上 */
        script_log_page_close();
        /* 先切到新页再删旧页：删除正在显示的屏会把 display 的 act_scr 置空，
         * 之后任何 lv_screen_load* 都会空指针崩溃。 */
        fw_window_switch_to(page, LV_SCREEN_LOAD_ANIM_NONE, 0);
        if (old != NULL) {
            lv_obj_delete(old);
        }
        /* 旧页上的控件全没了：登记表清空，旧句柄之后一律当无效（下面登记的只是新页） */
        widgets_reset();
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

    if (!widget_alive(parent)) {
        lua_pushnil(L);
        return 1;
    }

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

    if (!widget_alive(parent)) {
        lua_pushnil(L);
        return 1;
    }

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

    if (!widget_alive(parent)) {
        lua_pushnil(L);
        return 1;
    }

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

    if (!widget_alive(parent)) {
        lua_pushnil(L);
        return 1;
    }

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
    if (widget_alive(obj)) {
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
    if (widget_alive(obj)) {
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
    if (widget_alive(obj) && lv_obj_check_type(obj, &lv_image_class)) {
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
    if (widget_alive(obj)) {
        lv_obj_t *inner = lv_obj_get_child(obj, 1);
        if (inner != NULL && lv_obj_check_type(inner, &lv_bar_class)) {
            fw_ui_progress_set(obj, (uint8_t)pct);  /* 内部把 > 100 截到 100 */
        }
    }
    lvgl_port_unlock();
    return 0;
}

/* ---------------------------------- 画布 ---------------------------------- */

/* 颜色统一用 0xRRGGBB 整数；打包方式与图库 App 解 BMP 的 RGB565 一致（面板刷新时统一交换） */
static uint16_t canvas_rgb565(uint32_t rgb)
{
    const uint16_t r = (uint16_t)((rgb >> 16) & 0xFFu);
    const uint16_t g = (uint16_t)((rgb >> 8) & 0xFFu);
    const uint16_t b = (uint16_t)(rgb & 0xFFu);

    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* 控件不是画布（或已经失效）时返回 NULL：方法表是所有控件共用的 */
static script_canvas_t *canvas_of(lv_obj_t *obj)
{
    if (obj == NULL || !widget_alive(obj)) return NULL;
    if (!lv_obj_check_type(obj, &lv_image_class)) return NULL;

    return (script_canvas_t *)lv_obj_get_user_data(obj);
}

static void canvas_px(script_canvas_t *c, int x, int y, uint16_t color)
{
    const int w = (int)c->dsc.header.w;
    const int h = (int)c->dsc.header.h;
    if (x < 0 || y < 0 || x >= w || y >= h) return;

    ((uint16_t *)c->px)[(size_t)y * (size_t)w + (size_t)x] = color;
}

/* ui.canvas(parent, w, h)：w 必须是偶数（RGB565 一行 4 字节对齐，否则 LVGL 会自己拷一份，
 * 之后改像素就不显示在屏幕上了） */
static int l_ui_canvas(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const lua_Integer w = luaL_checkinteger(L, 2);
    const lua_Integer h = luaL_checkinteger(L, 3);

    if (!widget_alive(parent) || w <= 0 || h <= 0 || (w % 2) != 0 ||
        w > SCRIPT_CANVAS_MAX_W || h > SCRIPT_CANVAS_MAX_H ||
        s_canvas_count >= SCRIPT_CANVAS_MAX) {
        lua_pushnil(L);
        return 1;
    }

    script_canvas_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        lua_pushnil(L);
        return 1;
    }

    c->px = calloc(1, (size_t)w * 2u * (size_t)h);      /* 初始全黑 */
    if (c->px == NULL) {
        free(c);
        lua_pushnil(L);
        return 1;
    }

    c->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    c->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    c->dsc.header.w      = (uint32_t)w;
    c->dsc.header.h      = (uint32_t)h;
    c->dsc.header.stride = (uint32_t)w * 2u;
    c->dsc.data_size     = (uint32_t)w * 2u * (uint32_t)h;
    c->dsc.data          = c->px;

    lvgl_port_lock(0);
    lv_obj_t *img = lv_image_create(parent);
    if (img != NULL) {
        lv_image_set_src(img, &c->dsc);
        lv_obj_set_user_data(img, c);                   /* 画布数据从这里捞 */
    }
    lvgl_port_unlock();

    if (img == NULL) {
        free(c->px);
        free(c);
        lua_pushnil(L);
        return 1;
    }

    s_canvases[s_canvas_count++] = c;
    return push_widget(L, img);
}

/* 画布坐标：脚本里画图基本都是 sin/cos 算出来的，必然是小数，所以坐标参数收任意数字
 * 并四舍五入（用 luaL_checkinteger 会直接报 "number has no integer representation"，
 * 指针时钟这种表盘就画不出来）。 */
static int check_coord(lua_State *L, int idx)
{
    return (int)lround(luaL_checknumber(L, idx));
}

static int l_widget_fill(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const uint32_t rgb = (uint32_t)luaL_checkinteger(L, 2);

    lvgl_port_lock(0);
    script_canvas_t *c = canvas_of(obj);
    if (c != NULL) {
        const uint16_t col = canvas_rgb565(rgb);
        const size_t n = (size_t)c->dsc.header.w * (size_t)c->dsc.header.h;
        for (size_t i = 0; i < n; i++) ((uint16_t *)c->px)[i] = col;
        lv_obj_invalidate(obj);
    }
    lvgl_port_unlock();
    return 0;
}

static int l_widget_rect(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const lua_Integer x = check_coord(L, 2);
    const lua_Integer y = check_coord(L, 3);
    const lua_Integer w = check_coord(L, 4);
    const lua_Integer h = check_coord(L, 5);
    const uint32_t rgb = (uint32_t)luaL_checkinteger(L, 6);

    lvgl_port_lock(0);
    script_canvas_t *c = canvas_of(obj);
    if (c != NULL) {
        const uint16_t col = canvas_rgb565(rgb);
        /* 先夹到画布范围内：脚本一个手误（w/h 写成几千）就会让下面这两层 C 循环转上
         * 百万圈，而它是在 LVGL 锁里跑的，界面会跟着一起冻住 */
        const lua_Integer cw = (lua_Integer)c->dsc.header.w;
        const lua_Integer ch = (lua_Integer)c->dsc.header.h;
        const lua_Integer x0 = (x < 0) ? 0 : ((x > cw) ? cw : x);
        const lua_Integer y0 = (y < 0) ? 0 : ((y > ch) ? ch : y);
        const lua_Integer x1 = (w > cw - x0) ? cw : x0 + w;
        const lua_Integer y1 = (h > ch - y0) ? ch : y0 + h;

        for (lua_Integer yy = y0; yy < y1; yy++) {
            for (lua_Integer xx = x0; xx < x1; xx++) {
                canvas_px(c, (int)xx, (int)yy, col);
            }
        }
        lv_obj_invalidate(obj);
    }
    lvgl_port_unlock();
    return 0;
}

static int l_widget_line(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    lua_Integer x0 = check_coord(L, 2);
    lua_Integer y0 = check_coord(L, 3);
    const lua_Integer x1 = check_coord(L, 4);
    const lua_Integer y1 = check_coord(L, 5);
    const uint32_t rgb = (uint32_t)luaL_checkinteger(L, 6);

    lvgl_port_lock(0);
    script_canvas_t *c = canvas_of(obj);
    if (c != NULL) {
        const uint16_t col = canvas_rgb565(rgb);
        /* Bresenham，整数逐点 */
        const lua_Integer dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
        const lua_Integer dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
        const lua_Integer sx = (x0 < x1) ? 1 : -1;
        const lua_Integer sy = (y0 < y1) ? 1 : -1;
        lua_Integer err = dx - dy;

        while (true) {
            canvas_px(c, (int)x0, (int)y0, col);
            if (x0 == x1 && y0 == y1) break;

            const lua_Integer e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
        lv_obj_invalidate(obj);
    }
    lvgl_port_unlock();
    return 0;
}

static int l_widget_circle(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const lua_Integer cx = check_coord(L, 2);
    const lua_Integer cy = check_coord(L, 3);
    const lua_Integer r = check_coord(L, 4);
    const uint32_t rgb = (uint32_t)luaL_checkinteger(L, 5);

    lvgl_port_lock(0);
    script_canvas_t *c = canvas_of(obj);
    if (c != NULL && r > 0) {
        const uint16_t col = canvas_rgb565(rgb);
        /* 每 2 度画一个点：圆周上看不出断点 */
        for (int a = 0; a < 360; a += 2) {
            const float rad = (float)a * 3.14159265f / 180.0f;
            const int x = (int)((float)cx + cosf(rad) * (float)r);
            const int y = (int)((float)cy + sinf(rad) * (float)r);
            canvas_px(c, x, y, col);
        }
        lv_obj_invalidate(obj);
    }
    lvgl_port_unlock();
    return 0;
}

/* ------------------------------ 绑定：输入 / 对话框 / 列表 ------------------------------ */

/* 输入框与它的键盘：键盘挂在脚本页根上、点输入框才弹（与 App 浮层键盘同一套做法），
 * 一个脚本页面共用一个键盘，页面销毁时随之回收（见 widgets_reset）。 */
#define SCRIPT_KB_HEIGHT   120
#define SCRIPT_INPUT_H     36

static void keyboard_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;

    if (s_kb != NULL) lv_obj_set_hidden(s_kb, true);

    /* 回车 = 确认：把该输入框注册的回调投给脚本任务（取消只收键盘） */
    if (code == LV_EVENT_READY && s_kb_for != NULL && s_cb_queue != NULL) {
        const int ref = (int)(intptr_t)lv_obj_get_user_data(s_kb_for);
        if (ref != LUA_NOREF) xQueueSend(s_cb_queue, &ref, 0);       /* 满就丢 */
    }
}

static void input_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_FOCUSED) return;

    lv_obj_t *ta = lv_event_get_target(e);
    if (s_page == NULL) return;

    if (s_kb == NULL) {
        s_kb = lv_keyboard_create(s_page);
        lv_obj_set_size(s_kb, lv_pct(100), SCRIPT_KB_HEIGHT);
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(s_kb, keyboard_event_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_kb, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    }

    s_kb_for = ta;
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_set_hidden(s_kb, false);
}

/* ui.input(parent[, placeholder[, on_ready]])：单行输入框，回车触发 on_ready */
static int l_ui_input(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const char *placeholder = luaL_optstring(L, 2, "");

    int ref = LUA_NOREF;
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        luaL_checktype(L, 3, LUA_TFUNCTION);
        lua_pushvalue(L, 3);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    if (!widget_alive(parent)) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *ta = lv_textarea_create(parent);
    if (ta != NULL) {
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, placeholder);
        lv_obj_set_size(ta, lv_pct(100), SCRIPT_INPUT_H);
        lv_obj_set_style_text_font(ta, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(ta, fw_theme_color_text_primary(), 0);
        lv_obj_set_user_data(ta, (void *)(intptr_t)ref);
        lv_obj_add_event_cb(ta, input_event_cb, LV_EVENT_FOCUSED, NULL);
    }
    lvgl_port_unlock();

    if (ta == NULL) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, ta);
}

/* 脚本对话框：确定 / 取消各一个回调，回调不带参数（脚本自己知道注册的是哪个） */
typedef struct {
    int ok_ref;
    int cancel_ref;
} script_dialog_t;

static void script_dialog_cb(fw_dialog_btn_t btn, void *user)
{
    script_dialog_t *d = (script_dialog_t *)user;
    if (d == NULL) return;

    const int ref = (btn == FW_DIALOG_BTN_OK) ? d->ok_ref : d->cancel_ref;
    if (ref != LUA_NOREF && s_cb_queue != NULL) {
        xQueueSend(s_cb_queue, &ref, 0);            /* 满就丢 */
    }
    free(d);                                        /* 引用留在注册表里，脚本停止时随状态一起释放 */
}

/* ui.dialog(title, msg[, on_ok[, on_cancel]])：模态确认框；没给 on_cancel 就不显示取消键 */
static int l_ui_dialog(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    const char *msg = luaL_checkstring(L, 2);

    script_dialog_t *d = calloc(1, sizeof(*d));
    if (d == NULL) return luaL_error(L, "out of memory");
    d->ok_ref = LUA_NOREF;
    d->cancel_ref = LUA_NOREF;

    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        luaL_checktype(L, 3, LUA_TFUNCTION);
        lua_pushvalue(L, 3);
        d->ok_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
        luaL_checktype(L, 4, LUA_TFUNCTION);
        lua_pushvalue(L, 4);
        d->cancel_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    fw_dialog_btn_t buttons = FW_DIALOG_BTN_OK;
    if (d->cancel_ref != LUA_NOREF) buttons |= FW_DIALOG_BTN_CANCEL;

    if (fw_ui_dialog(NULL, title, msg, buttons, script_dialog_cb, d) == NULL) {
        free(d);
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, true);
    return 1;
}

/* ui.list(parent[, title])：可滚动的列表容器（与 App 里同一个组件，自己滚） */
static int l_ui_list(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const char *title = luaL_optstring(L, 2, NULL);

    if (!widget_alive(parent)) {
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *box = fw_ui_list(parent, title);
    if (box != NULL) {
        lv_obj_set_width(box, lv_pct(100));
        lv_obj_set_flex_grow(box, 1);
    }
    lvgl_port_unlock();

    if (box == NULL) {
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, box);
}

/* ui.item(list, text[, value[, on_click]])：列表里的一项 */
static int l_ui_item(lua_State *L)
{
    lv_obj_t *list = check_widget(L, 1);
    const char *text = luaL_checkstring(L, 2);
    const char *value = luaL_optstring(L, 3, NULL);

    int ref = LUA_NOREF;
    if (lua_gettop(L) >= 4 && !lua_isnil(L, 4)) {
        luaL_checktype(L, 4, LUA_TFUNCTION);
        lua_pushvalue(L, 4);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    if (!widget_alive(list)) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *item = fw_ui_list_add_value(list, text, value, widget_event_cb, NULL);
    if (item != NULL) {
        lv_obj_set_user_data(item, (void *)(intptr_t)ref);
    }
    lvgl_port_unlock();

    if (item == NULL) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, item);
}

/* ui.button(parent, w, h, text[, on_click])：指定大小的普通按钮（配 ui.grid 就是格子） */
static int l_ui_button(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const lua_Integer w = luaL_checkinteger(L, 2);
    const lua_Integer h = luaL_checkinteger(L, 3);
    const char *text = luaL_checkstring(L, 4);

    int ref = LUA_NOREF;
    if (lua_gettop(L) >= 5 && !lua_isnil(L, 5)) {
        luaL_checktype(L, 5, LUA_TFUNCTION);
        lua_pushvalue(L, 5);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    if (!widget_alive(parent)) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *btn = lv_button_create(parent);
    if (btn != NULL) {
        lv_obj_set_size(btn, (int32_t)((w > 0) ? w : 80), (int32_t)((h > 0) ? h : 36));
        lv_obj_set_style_bg_color(btn, fw_theme_color_bg_card(), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, fw_theme_color_border(), 0);

        lv_obj_t *lb = lv_label_create(btn);
        lv_label_set_text(lb, text);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(lb, fw_asset_font_cn(), 0);
        lv_obj_set_style_text_color(lb, fw_theme_color_text_primary(), 0);
        lv_obj_center(lb);

        lv_obj_set_user_data(btn, (void *)(intptr_t)ref);
        if (ref != LUA_NOREF) {
            lv_obj_add_event_cb(btn, widget_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
        }
    }
    lvgl_port_unlock();

    if (btn == NULL) {
        if (ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, btn);
}

/* ui.box(parent, w, h)：普通容器（透明底、无描边），用来分组摆放子控件 */
static int l_ui_box(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const lua_Integer w = luaL_checkinteger(L, 2);
    const lua_Integer h = luaL_checkinteger(L, 3);

    if (!widget_alive(parent)) {
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *o = lv_obj_create(parent);
    if (o != NULL) {
        lv_obj_set_size(o, (int32_t)((w > 0) ? w : lv_pct(100)), (int32_t)h);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_pad_all(o, 0, 0);
        lv_obj_set_style_pad_row(o, 6, 0);
        lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(o, false);
    }
    lvgl_port_unlock();

    if (o == NULL) {
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, o);
}

/* ui.grid(parent, cols, item_w, item_h)：每行 cols 个、格子 item_w × item_h 的流式容器，
 * 往里放 ui.button / ui.label / ui.image 就是网格 */
static int l_ui_grid(lua_State *L)
{
    lv_obj_t *parent = check_widget(L, 1);
    const lua_Integer cols = luaL_checkinteger(L, 2);
    const lua_Integer iw = luaL_checkinteger(L, 3);
    const lua_Integer ih = luaL_checkinteger(L, 4);

    if (!widget_alive(parent) || cols <= 0 || cols > 8 || iw <= 0 || ih <= 0) {
        lua_pushnil(L);
        return 1;
    }

    lvgl_port_lock(0);
    lv_obj_t *grid = fw_ui_grid(parent, (uint8_t)cols, (int32_t)iw, (int32_t)ih);
    lvgl_port_unlock();

    if (grid == NULL) {
        lua_pushnil(L);
        return 1;
    }
    return push_widget(L, grid);
}

/* ------------------------------- 通用方法 ------------------------------- */

/* 取文本：输入框与标签都支持 */
static int l_widget_get_text(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    bool ok = false;

    lvgl_port_lock(0);
    if (widget_alive(obj)) {
        if (lv_obj_check_type(obj, &lv_textarea_class)) {
            lua_pushstring(L, lv_textarea_get_text(obj));
            ok = true;
        } else if (lv_obj_check_type(obj, &lv_label_class)) {
            lua_pushstring(L, lv_label_get_text(obj));
            ok = true;
        }
    }
    lvgl_port_unlock();

    if (!ok) lua_pushnil(L);
    return 1;
}

/* 改尺寸：图片会切成"适应框"，所以改尺寸就是缩放 */
static int l_widget_set_size(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const lua_Integer w = luaL_checkinteger(L, 2);
    const lua_Integer h = luaL_checkinteger(L, 3);

    lvgl_port_lock(0);
    if (widget_alive(obj)) {
        lv_obj_set_size(obj, (int32_t)w, (int32_t)h);
        if (lv_obj_check_type(obj, &lv_image_class)) {
            lv_image_set_inner_align(obj, LV_IMAGE_ALIGN_STRETCH);
        }
    }
    lvgl_port_unlock();
    return 0;
}

/* 改背景色（0xRRGGBB） */
static int l_widget_set_color(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const uint32_t rgb = (uint32_t)luaL_checkinteger(L, 2);

    lvgl_port_lock(0);
    if (widget_alive(obj)) {
        lv_obj_set_style_bg_color(obj, lv_color_hex(rgb), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    lvgl_port_unlock();
    return 0;
}

/* 改文字色：按钮 / 卡片里第一层是文字标签的话一起改，免得白字白底看不见 */
static int l_widget_set_text_color(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const uint32_t rgb = (uint32_t)luaL_checkinteger(L, 2);

    lvgl_port_lock(0);
    if (widget_alive(obj)) {
        lv_obj_set_style_text_color(obj, lv_color_hex(rgb), 0);

        lv_obj_t *child = lv_obj_get_child(obj, 0);
        if (child != NULL && lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_style_text_color(child, lv_color_hex(rgb), 0);
        }
    }
    lvgl_port_unlock();
    return 0;
}

static int l_widget_set_radius(lua_State *L)
{
    lv_obj_t *obj = check_widget(L, 1);
    const lua_Integer r = luaL_checkinteger(L, 2);

    lvgl_port_lock(0);
    if (widget_alive(obj)) {
        lv_obj_set_style_radius(obj, (int32_t)((r < 0) ? 0 : r), 0);
    }
    lvgl_port_unlock();
    return 0;
}

static const luaL_Reg widget_methods[] = {
    { "set_text", l_widget_set_text },
    { "set_value", l_widget_set_value },
    { "set_src", l_widget_set_src },
    { "set", l_widget_set },
    { "get_text", l_widget_get_text },
    { "set_size", l_widget_set_size },
    { "set_color", l_widget_set_color },
    { "set_text_color", l_widget_set_text_color },
    { "set_radius", l_widget_set_radius },
    { "fill", l_widget_fill },
    { "rect", l_widget_rect },
    { "line", l_widget_line },
    { "circle", l_widget_circle },
    { NULL, NULL },
};

static const luaL_Reg ui_lib[] = {
    { "toast", l_ui_toast },
    { "page", l_ui_page },
    { "label", l_ui_label },
    { "row", l_ui_row },
    { "image", l_ui_image },
    { "progress", l_ui_progress },
    { "canvas", l_ui_canvas },
    { "input", l_ui_input },
    { "dialog", l_ui_dialog },
    { "list", l_ui_list },
    { "item", l_ui_item },
    { "button", l_ui_button },
    { "box", l_ui_box },
    { "grid", l_ui_grid },
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
        script_pcall(s_L, 0, 0);
    }
}

/* ------------------------------ 绑定：event ------------------------------ */

#define SCRIPT_EVENT_MAX    16

typedef struct {
    const char *name;
    svc_event_id_t id;
} script_event_map_t;

static const script_event_map_t EVENT_MAP[] = {
    { "touch",           SVC_EVENT_TOUCH },
    /* BOOT 键：回调参数是 1 字节字符串，string.byte(v) 得到事件序号
     * （0 单击 / 1 双击 / 2 长按） */
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
    { "script_failed",   SVC_EVENT_SCRIPT_FAILED },
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
    svc_event_id_t id;      /* 事件号：触摸这类带结构体负载的要按 id 解释 data */
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
    out.id = evt->id;
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

        /* 触摸事件负载是 svc_touch_point_t：拆成 {x, y, pressed} 表给脚本用
         * （画布上的游戏靠它判断点在哪边）；其余事件照旧按字符串传 */
        if (ev.id == SVC_EVENT_TOUCH && ev.len == sizeof(svc_touch_point_t)) {
            const svc_touch_point_t *p = (const svc_touch_point_t *)ev.data;
            lua_createtable(s_L, 0, 3);
            lua_pushinteger(s_L, p->x);
            lua_setfield(s_L, -2, "x");
            lua_pushinteger(s_L, p->y);
            lua_setfield(s_L, -2, "y");
            lua_pushboolean(s_L, p->pressed);
            lua_setfield(s_L, -2, "pressed");
            script_pcall(s_L, 1, 0);
            continue;
        }

        if (ev.len > 0) {
            lua_pushlstring(s_L, (const char *)ev.data, ev.len);
        } else {
            lua_pushnil(s_L);
        }
        script_pcall(s_L, 1, 0);
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
        script_pcall(s_L, nargs, 0);
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
    svc_imu_data_t d;
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

/* ------------------------------ 绑定：util ------------------------------
 *
 * 纯计算的文本工具：base64 与十六进制编解码。拼 HTTP 头（Basic 认证）、
 * 把二进制塞进文本协议、调试打印都常用，纯 C 实现不依赖外部库。
 */

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int l_util_base64_encode(lua_State *L)
{
    size_t len = 0;
    const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &len);

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) |
                           (((i + 1 < len) ? (uint32_t)in[i + 1] : 0) << 8) |
                           ((i + 2 < len) ? (uint32_t)in[i + 2] : 0);
        const char out[4] = {
            B64_CHARS[(v >> 18) & 0x3F],
            B64_CHARS[(v >> 12) & 0x3F],
            (i + 1 < len) ? B64_CHARS[(v >> 6) & 0x3F] : '=',
            (i + 2 < len) ? B64_CHARS[v & 0x3F] : '=',
        };
        luaL_addlstring(&b, out, sizeof(out));
    }

    luaL_pushresult(&b);
    return 1;
}

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int l_util_base64_decode(lua_State *L)
{
    size_t len = 0;
    const char *in = luaL_checklstring(L, 1, &len);

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    uint32_t acc = 0;
    int bits = 0;

    for (size_t i = 0; i < len; i++) {
        const int v = b64_value((unsigned char)in[i]);
        if (v < 0) continue;                    /* 填充 / 换行 / 空白一律跳过 */

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            const char c = (char)((acc >> bits) & 0xFF);
            luaL_addlstring(&b, &c, 1);
        }
    }

    luaL_pushresult(&b);
    return 1;
}

static int l_util_hex_encode(lua_State *L)
{
    static const char hex[] = "0123456789abcdef";

    size_t len = 0;
    const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &len);

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    for (size_t i = 0; i < len; i++) {
        const char out[2] = { hex[in[i] >> 4], hex[in[i] & 0x0F] };
        luaL_addlstring(&b, out, sizeof(out));
    }

    luaL_pushresult(&b);
    return 1;
}

static int l_util_hex_decode(lua_State *L)
{
    size_t len = 0;
    const char *in = luaL_checklstring(L, 1, &len);

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    int hi = -1;
    for (size_t i = 0; i < len; i++) {
        const int c = (unsigned char)in[i];
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;

        if (hi < 0) {
            hi = v;
            continue;
        }
        const char out = (char)((hi << 4) | v);
        luaL_addlstring(&b, &out, 1);
        hi = -1;
    }

    luaL_pushresult(&b);
    return 1;
}

static const luaL_Reg util_lib[] = {
    { "base64_encode", l_util_base64_encode },
    { "base64_decode", l_util_base64_decode },
    { "hex_encode", l_util_hex_encode },
    { "hex_decode", l_util_hex_decode },
    { NULL, NULL },
};

static int luaopen_util(lua_State *L)
{
    luaL_newlib(L, util_lib);
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
    lua_Integer sec = SCRIPT_BT_SCAN_SEC_DEFAULT;
    if (lua_gettop(L) >= 1) sec = luaL_checkinteger(L, 1);
    if (sec < 0) sec = 0;                   /* 非法值交给服务侧按默认时长处理 */
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
        char bda[18];
        snprintf(bda, sizeof(bda), "%02x:%02x:%02x:%02x:%02x:%02x",
                 res[i].bda[0], res[i].bda[1], res[i].bda[2],
                 res[i].bda[3], res[i].bda[4], res[i].bda[5]);

        lua_createtable(L, 0, 4);
        lua_pushstring(L, res[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, bda);
        lua_setfield(L, -2, "bda");             /* 连接时原样传给 bt.gatt_connect() */
        lua_pushinteger(L, res[i].addr_type);    /* 0 = public，1 = random（手机多为 1） */
        lua_setfield(L, -2, "addr_type");
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

/* -------------------- bt.gatt：中心角色（GATT 客户端） --------------------
 *
 * 连接 / 发现 / 读写订阅都是异步的：绑定函数把"这次在等什么"记在 s_gatt_op 上，
 * 由 svc_bt_central 的回调（跑在 BTC 任务里）填结果并释放 s_gatt_sem，脚本任务在
 * 绑定函数里阻塞等待（超时返回 nil / false）。通知不阻塞：回调把「句柄 + 数据」
 * 投进 s_gatt_queue，script_task 取出后调用 bt.gatt_on_notify() 注册的函数。
 *
 * 阻塞的是 script_task 自己（不是 LVGL 任务，也没纳入看门狗），所以不会卡界面；
 * 代价是等待期间脚本的定时器与其它事件会延后。
 *
 * UUID 参数可以是数字（16 位）或字符串（"0x180F" / 完整 128 位
 * "0000180f-0000-1000-8000-00805f9b34fb"），与 bt.gatt_services() / gatt_chars()
 * 给出的字符串形式一致，能回环使用。
 */

#define SCRIPT_GATT_TIMEOUT_DEFAULT 2000    /* 普通操作默认等待时长 */
#define SCRIPT_GATT_DISCOVER_MS     5000    /* 服务发现默认等待时长 */
#define SCRIPT_GATT_DATA_MAX        64      /* 读到的值 / 通知载荷上限（超出截断） */
#define SCRIPT_GATT_SVC_MAX         16
#define SCRIPT_GATT_CHAR_MAX        16
#define SCRIPT_GATT_NOTIFY_QUEUE_LEN 4

typedef struct {
    int ref;
    uint16_t handle;
    uint32_t len;
    uint8_t data[SCRIPT_GATT_DATA_MAX];
} script_gatt_msg_t;

typedef enum {
    GATT_OP_NONE = 0,
    GATT_OP_DISCOVER,
    GATT_OP_READ,
    GATT_OP_WRITE,
    GATT_OP_SUBSCRIBE,
} gatt_op_t;

static SemaphoreHandle_t s_gatt_sem = NULL;
static QueueHandle_t s_gatt_queue = NULL;
static int s_gatt_notify_ref = LUA_NOREF;
static volatile gatt_op_t s_gatt_op = GATT_OP_NONE;
static volatile esp_err_t s_gatt_err = ESP_OK;
static uint8_t s_gatt_data[SCRIPT_GATT_DATA_MAX];
static size_t s_gatt_len = 0;
static bool s_gatt_script_link = false;     /* 中心连接是脚本自己发起的（停止时负责断开） */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 解析 Lua 侧的 UUID：数字或字符串 → svc_bt_uuid_t */
static bool uuid_parse(lua_State *L, int idx, svc_bt_uuid_t *out)
{
    memset(out, 0, sizeof(*out));

    if (lua_type(L, idx) == LUA_TNUMBER) {
        const lua_Integer v = lua_tointeger(L, idx);
        if (v < 0 || v > 0xFFFF) return false;
        out->uuid16 = (uint16_t)v;
        return true;
    }

    const char *s = luaL_checkstring(L, idx);

    if (strchr(s, '-') == NULL) {
        /* 16 位：允许 "0x180F" 或 "180f" */
        char *end = NULL;
        const unsigned long v = strtoul(s, &end, 16);
        if (end == s || end == NULL || *end != '\0' || v > 0xFFFF) return false;
        out->uuid16 = (uint16_t)v;
        return true;
    }

    /* 128 位：8-4-4-4-12，去掉连接符后必须是 32 个十六进制字符 */
    uint8_t digits[32];
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '-') continue;
        if (n >= sizeof(digits)) return false;
        digits[n++] = (uint8_t)*p;
    }
    if (n != sizeof(digits)) return false;

    uint8_t canon[16];
    for (size_t i = 0; i < sizeof(canon); i++) {
        const int hi = hex_val((char)digits[2 * i]);
        const int lo = hex_val((char)digits[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        canon[i] = (uint8_t)((hi << 4) | lo);
    }

    /* Bluedroid 的 uuid128 与字符串顺序相反（小端），与资源里的 128 位常量一致 */
    out->is_128 = true;
    for (size_t i = 0; i < sizeof(canon); i++) out->uuid128[i] = canon[sizeof(canon) - 1 - i];
    return true;
}

static void uuid_format(const svc_bt_uuid_t *u, char *out, size_t len)
{
    if (!u->is_128) {
        snprintf(out, len, "0x%04X", u->uuid16);
        return;
    }

    uint8_t c[16];
    for (size_t i = 0; i < sizeof(c); i++) c[i] = u->uuid128[sizeof(c) - 1 - i];
    snprintf(out, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
             c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
}

static bool uuid_equal(const svc_bt_uuid_t *a, const svc_bt_uuid_t *b)
{
    if (a->is_128 != b->is_128) return false;
    if (a->is_128) return memcmp(a->uuid128, b->uuid128, sizeof(a->uuid128)) == 0;
    return a->uuid16 == b->uuid16;
}

/* "aa:bb:cc:dd:ee:ff" → 6 字节 */
static bool bda_parse(const char *s, uint8_t out[6])
{
    int n = 0;
    for (const char *p = s; *p != '\0';) {
        if (*p == ':' || *p == '-') {
            p++;
            continue;
        }

        const int hi = hex_val(*p++);
        const int lo = hex_val(*p++);
        if (hi < 0 || lo < 0 || n >= 6) return false;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n == 6;
}

/* 按 UUID 找已发现的服务（拿到句柄范围才能列特征） */
static bool central_find_svc(const svc_bt_uuid_t *uuid, svc_bt_central_service_t *out)
{
    svc_bt_central_service_t svcs[SCRIPT_GATT_SVC_MAX];
    size_t n = 0;

    if (svc_bt_central_get_services(svcs, SCRIPT_GATT_SVC_MAX, &n) != ESP_OK) return false;
    for (size_t i = 0; i < n; i++) {
        if (uuid_equal(&svcs[i].uuid, uuid)) {
            *out = svcs[i];
            return true;
        }
    }
    return false;
}

/* 发起异步操作前调用：清掉上一次超时后迟到的信号，并记下这次在等什么 */
static void gatt_begin(gatt_op_t op)
{
    if (s_gatt_sem != NULL) xSemaphoreTake(s_gatt_sem, 0);
    s_gatt_err = ESP_OK;
    s_gatt_op = op;
}

static void gatt_abort(void)
{
    s_gatt_op = GATT_OP_NONE;
}

/* 等结果；超时返回 false。之后迟到的结果会被 gatt_event_cb 忽略 */
static bool gatt_wait(uint32_t timeout_ms)
{
    if (s_gatt_sem == NULL) {
        gatt_abort();
        return false;
    }

    const TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    const bool got = (xSemaphoreTake(s_gatt_sem, ticks) == pdTRUE);
    const esp_err_t err = s_gatt_err;

    gatt_abort();
    return got && err == ESP_OK;
}

/* svc_bt_central 的事件回调（BTC 任务）：只填结果 / 投队列，不碰 Lua */
static void gatt_event_cb(const svc_bt_central_evt_data_t *evt, void *user)
{
    (void)user;

    switch (evt->evt) {
    case SVC_BT_CENTRAL_EVT_DISCOVER_DONE:
    case SVC_BT_CENTRAL_EVT_READ:
    case SVC_BT_CENTRAL_EVT_WRITE_DONE:
    case SVC_BT_CENTRAL_EVT_SUBSCRIBED: {
        const gatt_op_t want = (evt->evt == SVC_BT_CENTRAL_EVT_DISCOVER_DONE) ? GATT_OP_DISCOVER :
                               (evt->evt == SVC_BT_CENTRAL_EVT_READ)          ? GATT_OP_READ :
                               (evt->evt == SVC_BT_CENTRAL_EVT_WRITE_DONE)    ? GATT_OP_WRITE :
                                                                                GATT_OP_SUBSCRIBE;
        if (s_gatt_op != want) return;      /* 超时之后迟到的结果，丢掉 */

        s_gatt_err = evt->err;
        s_gatt_len = 0;
        if (want == GATT_OP_READ && evt->err == ESP_OK && evt->data != NULL) {
            s_gatt_len = (evt->len > sizeof(s_gatt_data)) ? sizeof(s_gatt_data) : evt->len;
            memcpy(s_gatt_data, evt->data, s_gatt_len);
        }
        if (s_gatt_sem != NULL) xSemaphoreGive(s_gatt_sem);
        break;
    }

    case SVC_BT_CENTRAL_EVT_NOTIFY:
        if (s_gatt_queue != NULL && s_gatt_notify_ref != LUA_NOREF) {
            script_gatt_msg_t out;
            memset(&out, 0, sizeof(out));
            out.ref = s_gatt_notify_ref;
            out.handle = evt->handle;
            out.len = (evt->len > sizeof(out.data)) ? (uint32_t)sizeof(out.data) : (uint32_t)evt->len;
            if (evt->data != NULL && out.len > 0) memcpy(out.data, evt->data, out.len);
            xQueueSend(s_gatt_queue, &out, 0);      /* 满就丢，不阻塞 BTC 任务 */
        }
        break;

    case SVC_BT_CENTRAL_EVT_DISCONNECTED:
        /* 断开时把还在等的操作叫醒，否则脚本要一直等到超时 */
        if (s_gatt_op != GATT_OP_NONE) {
            s_gatt_err = ESP_ERR_INVALID_STATE;
            if (s_gatt_sem != NULL) xSemaphoreGive(s_gatt_sem);
        }
        break;

    default:
        break;
    }
}

/* 在脚本任务里消费通知 */
static void drain_gatt_notifications(void)
{
    if (s_L == NULL || s_gatt_queue == NULL) return;

    script_gatt_msg_t msg;
    while (xQueueReceive(s_gatt_queue, &msg, 0) == pdTRUE) {
        lua_rawgeti(s_L, LUA_REGISTRYINDEX, msg.ref);
        lua_pushinteger(s_L, msg.handle);
        lua_pushlstring(s_L, (const char *)msg.data, msg.len);
        script_pcall(s_L, 2, 0);
    }
}

/* 停止 / 启动失败时回收中心角色的脚本侧状态 */
static void gatt_reset(void)
{
    if (s_gatt_notify_ref != LUA_NOREF) {
        if (s_L != NULL) luaL_unref(s_L, LUA_REGISTRYINDEX, s_gatt_notify_ref);
        s_gatt_notify_ref = LUA_NOREF;
    }
    gatt_abort();

    /* 队列里存的是本状态的函数引用，必须丢干净 */
    if (s_gatt_queue != NULL) {
        script_gatt_msg_t msg;
        while (xQueueReceive(s_gatt_queue, &msg, 0) == pdTRUE) { }
    }

    /* 中心连接是脚本自己发起的才断开（App 发起的连接不归脚本管） */
    if (s_gatt_script_link) {
        svc_bt_central_disconnect();
        s_gatt_script_link = false;
    }
}

/* 中心连接状态：'disconnected' / 'connecting' / 'connected' */
static int l_bt_gatt_state(lua_State *L)
{
    const char *name = "disconnected";

    svc_bt_central_status_t st;
    if (svc_bt_central_get_status(&st) == ESP_OK) {
        if (st.connected) name = "connected";
        else if (st.connecting) name = "connecting";
    }
    lua_pushstring(L, name);
    return 1;
}

static int l_bt_gatt_connect(lua_State *L)
{
    const char *bda = luaL_checkstring(L, 1);
    const lua_Integer addr_type = luaL_optinteger(L, 2, 0);

    uint8_t addr[6];
    if (!bda_parse(bda, addr) || addr_type < 0 || addr_type > 1) {
        lua_pushboolean(L, false);
        return 1;
    }

    const bool ok = (svc_bt_central_connect(addr, (uint8_t)addr_type) == ESP_OK);
    if (ok) s_gatt_script_link = true;
    lua_pushboolean(L, ok);
    return 1;
}

static int l_bt_gatt_disconnect(lua_State *L)
{
    (void)L;
    s_gatt_script_link = false;
    lua_pushboolean(L, svc_bt_central_disconnect() == ESP_OK);
    return 1;
}

static int l_bt_gatt_discover(lua_State *L)
{
    lua_Integer timeout = SCRIPT_GATT_DISCOVER_MS;
    if (lua_gettop(L) >= 1) timeout = luaL_checkinteger(L, 1);
    if (timeout < 0) timeout = 0;

    gatt_begin(GATT_OP_DISCOVER);
    if (svc_bt_central_discover() != ESP_OK) {
        gatt_abort();
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, gatt_wait((uint32_t)timeout));
    return 1;
}

static int l_bt_gatt_services(lua_State *L)
{
    svc_bt_central_service_t svcs[SCRIPT_GATT_SVC_MAX];
    size_t n = 0;
    if (svc_bt_central_get_services(svcs, SCRIPT_GATT_SVC_MAX, &n) != ESP_OK || n == 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        char uuid[40];
        uuid_format(&svcs[i].uuid, uuid, sizeof(uuid));

        lua_createtable(L, 0, 3);
        lua_pushstring(L, uuid);
        lua_setfield(L, -2, "uuid");
        lua_pushinteger(L, (lua_Integer)svcs[i].start_handle);
        lua_setfield(L, -2, "start");
        lua_pushinteger(L, (lua_Integer)svcs[i].end_handle);
        lua_setfield(L, -2, "end");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

/* gatt_chars(svc_uuid) → { { uuid=, handle=, props= }, ... }；需先 discover() */
static int l_bt_gatt_chars(lua_State *L)
{
    svc_bt_uuid_t svc;
    svc_bt_central_service_t found;
    if (!uuid_parse(L, 1, &svc) || !central_find_svc(&svc, &found)) {
        lua_pushnil(L);
        return 1;
    }

    svc_bt_central_char_t chars[SCRIPT_GATT_CHAR_MAX];
    size_t n = 0;
    if (svc_bt_central_get_chars(&found, chars, SCRIPT_GATT_CHAR_MAX, &n) != ESP_OK || n == 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, (int)n, 0);
    for (size_t i = 0; i < n; i++) {
        char uuid[40];
        uuid_format(&chars[i].uuid, uuid, sizeof(uuid));

        lua_createtable(L, 0, 3);
        lua_pushstring(L, uuid);
        lua_setfield(L, -2, "uuid");
        lua_pushinteger(L, (lua_Integer)chars[i].handle);
        lua_setfield(L, -2, "handle");
        lua_pushinteger(L, chars[i].properties);    /* SVC_BT_CHAR_PROP_* 位掩码 */
        lua_setfield(L, -2, "props");
        lua_rawseti(L, -2, (int)(i + 1));
    }
    return 1;
}

/* gatt_read(svc, chr[, timeout_ms]) → 字符串；失败 / 超时返回 nil */
static int l_bt_gatt_read(lua_State *L)
{
    svc_bt_uuid_t svc, chr;
    if (!uuid_parse(L, 1, &svc) || !uuid_parse(L, 2, &chr)) {
        lua_pushnil(L);
        return 1;
    }

    lua_Integer timeout = SCRIPT_GATT_TIMEOUT_DEFAULT;
    if (lua_gettop(L) >= 3) timeout = luaL_checkinteger(L, 3);
    if (timeout < 0) timeout = 0;

    gatt_begin(GATT_OP_READ);
    if (svc_bt_central_read(&svc, &chr) != ESP_OK) {
        gatt_abort();
        lua_pushnil(L);
        return 1;
    }

    if (!gatt_wait((uint32_t)timeout)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)s_gatt_data, s_gatt_len);
    return 1;
}

/* gatt_write(svc, chr, data[, with_response]) → bool（不带响应时只保证请求已发出） */
static int l_bt_gatt_write(lua_State *L)
{
    svc_bt_uuid_t svc, chr;
    if (!uuid_parse(L, 1, &svc) || !uuid_parse(L, 2, &chr)) {
        lua_pushboolean(L, false);
        return 1;
    }

    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);
    const bool with_response = lua_toboolean(L, 4);

    if (!with_response) {
        /* 无响应写入没有完成事件，只能报告"请求已发出" */
        lua_pushboolean(L, svc_bt_central_write(&svc, &chr, data, len, false) == ESP_OK);
        return 1;
    }

    gatt_begin(GATT_OP_WRITE);
    if (svc_bt_central_write(&svc, &chr, data, len, true) != ESP_OK) {
        gatt_abort();
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, gatt_wait(SCRIPT_GATT_TIMEOUT_DEFAULT));
    return 1;
}

static int l_bt_gatt_subscribe(lua_State *L)
{
    svc_bt_uuid_t svc, chr;
    if (!uuid_parse(L, 1, &svc) || !uuid_parse(L, 2, &chr)) {
        lua_pushboolean(L, false);
        return 1;
    }
    const bool on = lua_toboolean(L, 3);

    gatt_begin(GATT_OP_SUBSCRIBE);
    if (svc_bt_central_subscribe(&svc, &chr, on) != ESP_OK) {
        gatt_abort();
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, gatt_wait(SCRIPT_GATT_TIMEOUT_DEFAULT));
    return 1;
}

/* gatt_on_notify(fn)：fn(handle, data)；传 nil 取消。同一时刻只保留一个 */
static int l_bt_gatt_on_notify(lua_State *L)
{
    if (s_gatt_notify_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_gatt_notify_ref);
        s_gatt_notify_ref = LUA_NOREF;
    }

    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        lua_pushvalue(L, 1);
        s_gatt_notify_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
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
    { "gatt_connect", l_bt_gatt_connect },
    { "gatt_disconnect", l_bt_gatt_disconnect },
    { "gatt_state", l_bt_gatt_state },
    { "gatt_discover", l_bt_gatt_discover },
    { "gatt_services", l_bt_gatt_services },
    { "gatt_chars", l_bt_gatt_chars },
    { "gatt_read", l_bt_gatt_read },
    { "gatt_write", l_bt_gatt_write },
    { "gatt_subscribe", l_bt_gatt_subscribe },
    { "gatt_on_notify", l_bt_gatt_on_notify },
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

/* capture(path)：把当前帧存成 24 位 BMP（转码在 svc_camera 里，和相机 App 同一套）。
 * 以 JPEG 打开的相机（拍照模式）输出的是 JPEG 码流，直接落盘。 */
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

    esp_err_t err;
    if (frame.jpeg || frame.data == NULL) {
        err = svc_storage_write(path, frame.data, frame.len);
    } else {
        err = svc_camera_write_bmp(path, (const uint16_t *)frame.data, frame.width, frame.height);
    }

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

/* 脚本自己请求退出：和用户点「停止」走同一条收尾路径。
 * 推一个特殊错误值把当前 Lua 调用栈展开，免得 sys.exit() 之后继续执行
 * （例如写在循环里）；script_pcall / start_script 认得这个值，按正常退出处理。 */
static int l_sys_exit(lua_State *L)
{
    fw_script_stop();
    lua_pushlightuserdata(L, (void *)&s_exit_marker);
    return lua_error(L);
}

static const luaL_Reg sys_lib[] = {
    { "uptime", l_sys_uptime },
    { "now", l_sys_now },
    { "localtime", l_sys_localtime },
    { "mem", l_sys_mem },
    { "exit", l_sys_exit },
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

/* i2c_write_reg(addr, reg, data)：先写寄存器号再写数据（EEPROM / 传感器常见写法） */
static int l_io_i2c_write_reg(lua_State *L)
{
    const lua_Integer addr = luaL_checkinteger(L, 1);
    const lua_Integer reg = luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 3, &len);

    if (len > SCRIPT_I2C_READ_MAX) {
        return luaL_error(L, "data too long (max %d)", SCRIPT_I2C_READ_MAX);
    }

    uint8_t buf[SCRIPT_I2C_READ_MAX + 1];
    buf[0] = (uint8_t)reg;
    if (len > 0) memcpy(buf + 1, data, len);

    lua_pushboolean(L, svc_io_i2c_write((uint8_t)addr, buf, len + 1) == ESP_OK);
    return 1;
}

/* i2c_read_reg(addr, reg, len)：写寄存器号再读 len 字节。
 * 注意是两次独立传输（没有 repeated start），需要"写寄存器号后不放开总线"的器件
 * （部分传感器）不适用，那种情况请用 i2c_write / i2c_read 自己拼。 */
static int l_io_i2c_read_reg(lua_State *L)
{
    const lua_Integer addr = luaL_checkinteger(L, 1);
    const lua_Integer reg = luaL_checkinteger(L, 2);
    const lua_Integer len = luaL_checkinteger(L, 3);

    if (len <= 0 || len > SCRIPT_I2C_READ_MAX) {
        return luaL_error(L, "len must be 1..%d", SCRIPT_I2C_READ_MAX);
    }

    const uint8_t r = (uint8_t)reg;
    if (svc_io_i2c_write((uint8_t)addr, &r, 1) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    uint8_t buf[SCRIPT_I2C_READ_MAX];
    if (svc_io_i2c_read((uint8_t)addr, buf, (size_t)len) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, (size_t)len);
    return 1;
}
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

/* UART 占用外扩口 GPIO10 / GPIO11，与 PWM 互斥（服务侧会拒绝占用的引脚）。
 * data_bits 5-8、parity 0=无 1=偶 2=奇、stop_bits 1/2，取值非法由服务返回错误。 */
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

/* CAN（TWAI，占用外扩口 GPIO10 / GPIO11，需外接收发器）。
 * 发送用固定超时（总线忙时最多等 100 ms）；接收的等待时间由脚本给，0 表示一直等。 */
static int l_io_can_config(lua_State *L)
{
    const lua_Integer bitrate = luaL_checkinteger(L, 1);
    const bool listen_only = lua_toboolean(L, 2);

    if (bitrate <= 0) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, svc_io_can_config((uint32_t)bitrate, listen_only) == ESP_OK);
    return 1;
}

static int l_io_can_stop(lua_State *L)
{
    (void)L;
    lua_pushboolean(L, svc_io_can_stop() == ESP_OK);
    return 1;
}

/* can_send(id, data[, extended])：data 是 0~8 字节的字符串，id 为 11 位或 29 位 */
static int l_io_can_send(lua_State *L)
{
    const lua_Integer id = luaL_checkinteger(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    const bool extended = lua_toboolean(L, 3);

    if (id < 0 || id > 0x1FFFFFFF || len > 8) {
        lua_pushboolean(L, false);
        return 1;
    }

    svc_io_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id = (uint32_t)id;
    f.extended = extended;
    f.len = (uint8_t)len;
    if (len > 0) memcpy(f.data, data, len);

    lua_pushboolean(L, svc_io_can_send(&f, SCRIPT_CAN_TIMEOUT_MS) == ESP_OK);
    return 1;
}

/* can_receive([timeout_ms])：返回 id, data, extended；超时 / 未配置返回 nil */
static int l_io_can_receive(lua_State *L)
{
    lua_Integer timeout = SCRIPT_CAN_TIMEOUT_MS;
    if (lua_gettop(L) >= 1) timeout = luaL_checkinteger(L, 1);
    if (timeout < 0) timeout = 0;           /* 0 = 无限等待，与服务侧一致 */

    svc_io_can_frame_t f;
    if (svc_io_can_receive(&f, (uint32_t)timeout) != ESP_OK) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)f.id);
    lua_pushlstring(L, (const char *)f.data, f.len);
    lua_pushboolean(L, f.extended);
    return 3;
}

static const luaL_Reg io_lib[] = {
    { "gpio_write", l_io_gpio_write },
    { "gpio_read", l_io_gpio_read },
    { "pwm_set", l_io_pwm_set },
    { "pwm_stop", l_io_pwm_stop },
    { "adc_read", l_io_adc_read },
    { "i2c_write", l_io_i2c_write },
    { "i2c_read", l_io_i2c_read },
    { "i2c_write_reg", l_io_i2c_write_reg },
    { "i2c_read_reg", l_io_i2c_read_reg },
    { "uart_config", l_io_uart_config },
    { "uart_write", l_io_uart_write },
    { "uart_read", l_io_uart_read },
    { "can_config", l_io_can_config },
    { "can_stop", l_io_can_stop },
    { "can_send", l_io_can_send },
    { "can_receive", l_io_can_receive },
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

/* 追加写：不存在时创建，适合脚本自己记日志 */
static int l_file_append(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    lua_pushboolean(L, svc_storage_append(path, data, len) == ESP_OK);
    return 1;
}

/* 文件大小（字节）；不存在返回 nil */
static int l_file_size(lua_State *L)
{
    size_t size = 0;
    if (svc_storage_exists(luaL_checkstring(L, 1), &size) != ESP_OK) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, (lua_Integer)size);
    }
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
    { "append", l_file_append },
    { "size", l_file_size },
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
    svc_audio_format_t fmt = {
        .sample_rate = SVC_AUDIO_SR_16K, .bit_width = 16, .channels = 2,
    };
    if (lua_gettop(L) >= 1) {
        fmt.sample_rate = (svc_audio_sample_rate_t)luaL_checkinteger(L, 1);
    }
    if (lua_gettop(L) >= 2) {
        lua_Integer ch = luaL_checkinteger(L, 2);
        fmt.channels = (uint8_t)((ch == 1) ? 1 : 2);
    }
    lua_pushboolean(L, svc_audio_stream_start(&fmt) == ESP_OK);
    return 1;
}

/* Lua 字符串按字节传入，内容就是 16-bit PCM。
 * 写入超时给 1 s（不是无限等）：I2S 卡住时脚本能拿到 false 并及时收尾，
 * 否则 script_task 会永远停在这里，连停止命令都处理不了。 */
static int l_audio_write(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    if (len == 0) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, svc_audio_write((const uint8_t *)data, len, 1000) == ESP_OK);
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

/* 通用请求：opts（在栈上的第 opts_idx 个参数）可以是数字（老的 timeout 写法）
 * 或表 { timeout = ms, headers = "K: V\r\n...", content_type = "..." }。
 * 返回：成功给响应体字符串；失败给 nil + 原因（"HTTP 404" / 错误名）。
 * 阻塞调用（默认 15 s 超时）：脚本任务不纳入看门狗，靠指令数钩子兜死循环。 */
static int net_request(lua_State *L, const char *method, const char *url, const char *body,
                       size_t body_len, int opts_idx)
{
    char headers[SVC_HTTP_HEADERS_MAX] = { 0 };
    char content_type[64] = { 0 };
    uint32_t timeout = 15000;

    if (lua_type(L, opts_idx) == LUA_TNUMBER) {
        const lua_Integer t = luaL_checkinteger(L, opts_idx);
        if (t > 0) timeout = (uint32_t)t;
    } else if (lua_istable(L, opts_idx)) {
        lua_getfield(L, opts_idx, "timeout");
        if (lua_isinteger(L, -1)) {
            const lua_Integer t = lua_tointeger(L, -1);
            if (t > 0) timeout = (uint32_t)t;
        }
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "headers");
        if (lua_isstring(L, -1)) strlcpy(headers, lua_tostring(L, -1), sizeof(headers));
        lua_pop(L, 1);

        lua_getfield(L, opts_idx, "content_type");
        if (lua_isstring(L, -1)) strlcpy(content_type, lua_tostring(L, -1), sizeof(content_type));
        lua_pop(L, 1);
    }

    char *buf = malloc(SCRIPT_HTTP_BUF);
    if (buf == NULL) return luaL_error(L, "out of memory");

    const svc_http_request_t req = {
        .method = method,
        .url = url,
        .content_type = (content_type[0] != '\0') ? content_type : NULL,
        .body = body,
        .body_len = body_len,
        .headers = (headers[0] != '\0') ? headers : NULL,
    };

    const esp_err_t err = svc_http_request(&req, buf, SCRIPT_HTTP_BUF, timeout);
    if (err != ESP_OK) {
        const int status = svc_http_last_status();
        free(buf);
        lua_pushnil(L);
        if (status > 0) lua_pushfstring(L, "HTTP %d", status);
        else lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_pushstring(L, buf);
    free(buf);
    return 1;
}

static int l_net_http_get(lua_State *L)
{
    return net_request(L, "GET", luaL_checkstring(L, 1), NULL, 0, 2);
}

/* http_post(url, body[, opts])：body 原样发；JSON 用 json.encode 拼，
 * Content-Type 传 "application/json" */
static int l_net_http_post(lua_State *L)
{
    const char *url = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *body = luaL_checklstring(L, 2, &len);

    if (len > SVC_HTTP_BODY_MAX) {
        return luaL_error(L, "body too long (max %d)", SVC_HTTP_BODY_MAX);
    }

    return net_request(L, "POST", url, body, len, 3);
}

static const luaL_Reg net_lib[] = {
    { "http_get", l_net_http_get },
    { "http_post", l_net_http_post },
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
        s_timers[i].one_shot = false;
    }
}

/* 摘掉一个定时器槽（会 unref：只在脚本任务里、状态有效时调用） */
static void timer_cancel_slot(int i)
{
    if (s_timers[i].ref != LUA_NOREF && s_L != NULL) {
        luaL_unref(s_L, LUA_REGISTRYINDEX, s_timers[i].ref);
    }
    s_timers[i].ref = LUA_NOREF;
    s_timers[i].period_ms = 0;
    s_timers[i].next_us = 0;
    s_timers[i].one_shot = false;
}

static int timer_add(lua_State *L, bool one_shot)
{
    const lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms <= 0) return luaL_error(L, "interval must be > 0");

    for (int i = 0; i < SCRIPT_TIMER_MAX; i++) {
        if (s_timers[i].ref == LUA_NOREF) {
            lua_pushvalue(L, 2);
            s_timers[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            s_timers[i].period_ms = (uint32_t)ms;
            s_timers[i].next_us = esp_timer_get_time() + (int64_t)ms * 1000;
            s_timers[i].one_shot = one_shot;
            lua_pushinteger(L, i + 1);          /* id：传给 timer.cancel */
            return 1;
        }
    }
    return luaL_error(L, "too many timers (max %d)", SCRIPT_TIMER_MAX);
}

static int l_timer_every(lua_State *L)
{
    return timer_add(L, false);
}

static int l_timer_after(lua_State *L)
{
    return timer_add(L, true);
}

static int l_timer_cancel(lua_State *L)
{
    const lua_Integer id = luaL_checkinteger(L, 1);

    if (id < 1 || id > SCRIPT_TIMER_MAX || s_timers[id - 1].ref == LUA_NOREF) {
        lua_pushboolean(L, false);
        return 1;
    }

    timer_cancel_slot((int)id - 1);
    lua_pushboolean(L, true);
    return 1;
}

static const luaL_Reg timer_lib[] = {
    { "every", l_timer_every },
    { "after", l_timer_after },
    { "cancel", l_timer_cancel },
    { NULL, NULL },
};

static int luaopen_timer(lua_State *L)
{
    luaL_newlib(L, timer_lib);
    return 1;
}

/* ---------------------------- 脚本运行日志 ----------------------------
 *
 * 脚本的 print 抓到这里（无锁字节环），没有界面的脚本靠它显示「运行日志」页；
 * 有界面的脚本看自己的界面即可。print 同时照常打一条串口日志（标签 script），
 * 方便和串口监视 / 系统日志对照。脚本拿不到 io / os，能输出的就这一条路。
 */

#define SCRIPT_LOG_RING_SIZE   2048u     /* 2 的幂 */
#define SCRIPT_LOG_LINE_MAX    160u
#define SCRIPT_LOG_REFRESH_MS  500
#define SCRIPT_LOG_TAG         "script"

static char s_log_ring[SCRIPT_LOG_RING_SIZE];
static volatile uint32_t s_log_head = 0;

/* 每次启动脚本都从头记：页面上只显示这一次运行的输出 */
static void script_log_reset(void)
{
    s_log_head = 0;
}

static void script_log_push(const char *s, size_t len)
{
    uint32_t head = s_log_head;
    for (size_t i = 0; i < len; i++) {
        s_log_ring[head & (SCRIPT_LOG_RING_SIZE - 1)] = s[i];
        head++;
    }
    s_log_head = head;
}

/* 取最近的内容（读侧可能赶上半行，显示前跳过第一个换行即可） */
static size_t script_log_tail(char *buf, size_t len)
{
    if (buf == NULL || len < 2) return 0;

    const uint32_t head = s_log_head;
    const uint32_t avail = (head < SCRIPT_LOG_RING_SIZE) ? head : SCRIPT_LOG_RING_SIZE;
    const size_t n = (avail < (len - 1)) ? (size_t)avail : (len - 1);

    for (size_t i = 0; i < n; i++) {
        buf[i] = s_log_ring[(head - n + i) & (SCRIPT_LOG_RING_SIZE - 1)];
    }
    buf[n] = '\0';
    return n;
}

/* 脚本 print：按 Lua 规则把每个参数转成字符串（认 __tostring），空格分隔 */
static int l_script_print(lua_State *L)
{
    char line[SCRIPT_LOG_LINE_MAX];
    size_t len = 0;
    const int n = lua_gettop(L);

    for (int i = 1; i <= n; i++) {
        size_t l = 0;
        const char *s = luaL_tolstring(L, i, &l);       /* 往栈上压字符串，下面 pop */
        if (s == NULL) continue;

        if (i > 1 && len + 1 < sizeof(line)) line[len++] = ' ';

        const size_t room = sizeof(line) - 1 - len;
        const size_t copy = (l < room) ? l : room;
        memcpy(line + len, s, copy);
        len += copy;
        lua_pop(L, 1);
    }
    line[len] = '\0';

    script_log_push(line, len);
    script_log_push("\n", 1);
    ESP_LOGI(SCRIPT_LOG_TAG, "%s", line);
    return 0;
}

/* 日志页刷新（LVGL 定时器）：内容变了才重排；贴着底部时才跟着往下滚，
 * 与「系统日志」App 同一套做法 */
static void script_log_tick(lv_timer_t *t)
{
    (void)t;

    if (s_log_lb == NULL) return;
    if (s_log_buf == NULL) s_log_buf = malloc(SCRIPT_LOG_RING_SIZE + 1);
    if (s_log_buf == NULL) return;

    if (script_log_tail(s_log_buf, SCRIPT_LOG_RING_SIZE + 1) == 0) return;

    /* 开头可能截在半行上：从第一个换行之后开始；只有一行且还没换行就原样显示 */
    const char *text = s_log_buf;
    const char *nl = strchr(s_log_buf, '\n');
    if (nl != NULL && nl[1] != '\0') text = nl + 1;

    if (s_log_prev != NULL && strcmp(s_log_prev, text) == 0) return;   /* 没变：不重排 */

    const bool at_bottom = (s_log_box == NULL) || (lv_obj_get_scroll_bottom(s_log_box) <= 2);
    lv_label_set_text(s_log_lb, text);

    if (s_log_prev == NULL) s_log_prev = malloc(SCRIPT_LOG_RING_SIZE + 1);
    if (s_log_prev != NULL) strlcpy(s_log_prev, text, SCRIPT_LOG_RING_SIZE + 1);

    if (at_bottom && s_log_box != NULL) {
        lv_obj_update_layout(s_log_lb);                 /* 先算出新高度再滚到底 */
        lv_obj_scroll_to_y(s_log_box, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

/* 无界面脚本的「运行日志」页：头部一行是脚本名，下面是滚动的 print 输出。
 * 建好后它就是这个脚本的页面（s_page），离开它同样会停脚本 —— 有无界面逻辑一致 */
static void script_log_page_open(void)
{
    lvgl_port_lock(0);

    if (s_page != NULL) {               /* 脚本自己建了界面：不插日志页 */
        lvgl_port_unlock();
        return;
    }

    fw_window_sync_active();
    if (lv_screen_active() != s_page) {
        s_prev_screen = lv_screen_active();
    }

    lv_obj_t *content = NULL;
    lv_obj_t *page = fw_ui_page(&content);
    if (page == NULL || content == NULL) {
        lvgl_port_unlock();
        return;
    }

    /* 头部一行：脚本名（撑满）+ "运行日志" */
    lv_obj_t *head = lv_obj_create(content);
    lv_obj_set_size(head, lv_pct(100), 28);
    lv_obj_set_scrollable(head, false);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(head);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(name, s_current);
    lv_obj_set_style_text_font(name, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(name, fw_theme_color_text_primary(), 0);

    lv_obj_t *tag = lv_label_create(head);
    lv_label_set_text(tag, "运行日志");
    lv_obj_set_style_text_font(tag, fw_asset_font_cn(), 0);
    lv_obj_set_style_text_color(tag, fw_theme_color_accent(), 0);

    /* 日志正文：余下的高度都给它，自己滚动 */
    s_log_box = lv_obj_create(content);
    lv_obj_set_width(s_log_box, lv_pct(100));
    lv_obj_set_flex_grow(s_log_box, 1);
    lv_obj_set_style_bg_color(s_log_box, fw_theme_color_bg_card(), 0);
    lv_obj_set_style_border_width(s_log_box, 1, 0);
    lv_obj_set_style_border_color(s_log_box, fw_theme_color_border(), 0);
    lv_obj_set_style_radius(s_log_box, 10, 0);
    lv_obj_set_style_pad_all(s_log_box, 8, 0);

    s_log_lb = lv_label_create(s_log_box);
    lv_obj_set_width(s_log_lb, lv_pct(100));
    lv_label_set_long_mode(s_log_lb, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_log_lb, fw_asset_font_14(), 0);
    lv_obj_set_style_text_color(s_log_lb, fw_theme_color_text_secondary(), 0);
    lv_label_set_text(s_log_lb, "运行中…");

    free(s_log_prev);                   /* 新页：第一帧一定要写进去 */
    s_log_prev = NULL;

    s_page = page;
    fw_window_switch_to(page, LV_SCREEN_LOAD_ANIM_NONE, 0);
    widgets_reset();                    /* 这一页上没有脚本建的控件 */

    s_log_timer = lv_timer_create(script_log_tick, SCRIPT_LOG_REFRESH_MS, NULL);

    lvgl_port_unlock();
}

/* 关掉日志页的刷新定时器（页面本身由 s_page 的删除一并带走）；调用方须持 LVGL 锁 */
static void script_log_page_close(void)
{
    if (s_log_timer != NULL) {
        lv_timer_delete(s_log_timer);
        s_log_timer = NULL;
    }

    free(s_log_buf);
    s_log_buf = NULL;
    free(s_log_prev);
    s_log_prev = NULL;
    s_log_box = NULL;
    s_log_lb = NULL;
}

/* ------------------------------- 运行时 ------------------------------- */

/* 统一调用脚本回调：真错误由 script_error 收口；sys.exit() 用特殊错误值要求展开，
 * 那不是错误（它已经投过 CMD_STOP），静默吞掉即可 */
static void script_pcall(lua_State *L, int nargs, int nres)
{
    if (lua_pcall(L, nargs, nres, 0) == LUA_OK) return;

    if (lua_touserdata(L, -1) == (void *)&s_exit_marker) {
        lua_pop(L, 1);
        return;
    }

    script_error(lua_tostring(L, -1), true);
    lua_pop(L, 1);
}

static void run_due_timers(void)
{
    const int64_t now = esp_timer_get_time();

    for (int i = 0; i < SCRIPT_TIMER_MAX; i++) {
        if (s_timers[i].ref == LUA_NOREF) continue;
        if (now < s_timers[i].next_us) continue;

        s_timers[i].next_us = now + (int64_t)s_timers[i].period_ms * 1000;
        const bool one_shot = s_timers[i].one_shot;

        lua_rawgeti(s_L, LUA_REGISTRYINDEX, s_timers[i].ref);
        script_pcall(s_L, 0, 0);

        /* 一次性回调（timer.after）跑完就摘掉；回调里自己 cancel 过的话槽已经空了 */
        if (one_shot && s_timers[i].ref != LUA_NOREF) {
            timer_cancel_slot(i);
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

    /* print 换成自己的：一行抄进脚本日志环（无界面脚本的「运行日志」页显示它），
     * 同时照常进串口日志。脚本没有 io / os，输出的唯一出口就是它 */
    lua_pushcfunction(L, l_script_print);
    lua_setglobal(L, "print");
}

/* ------------------------------ 绑定：json ------------------------------
 *
 * 用 cJSON 编解码：net.http_post 发 JSON、解析 Web API 的响应都靠它。
 * 解码映射：对象 → 表、数组 → 数组表、null → nil、布尔 → boolean、
 * 数字 → number、字符串 → string（嵌套限 8 层，防深表把栈打爆）。
 */
#define SCRIPT_JSON_DEPTH_MAX 8

static void json_to_lua(lua_State *L, const cJSON *item, int depth)
{
    if (item == NULL || depth > SCRIPT_JSON_DEPTH_MAX) {
        lua_pushnil(L);
        return;
    }

    if (cJSON_IsObject(item)) {
        lua_createtable(L, 0, 8);
        for (const cJSON *c = item->child; c != NULL; c = c->next) {
            json_to_lua(L, c, depth + 1);
            lua_setfield(L, -2, (c->string != NULL) ? c->string : "");
        }
    } else if (cJSON_IsArray(item)) {
        int n = 0;
        for (const cJSON *c = item->child; c != NULL; c = c->next) n++;

        lua_createtable(L, n, 0);
        int i = 1;
        for (const cJSON *c = item->child; c != NULL; c = c->next) {
            json_to_lua(L, c, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
    } else if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, cJSON_GetNumberValue(item));
    } else if (cJSON_IsString(item)) {
        lua_pushstring(L, cJSON_GetStringValue(item));
    } else {
        lua_pushnil(L);                         /* null 与其它 */
    }
}

/* json.decode(str)：解析失败返回 nil */
static int l_json_decode(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);

    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        lua_pushnil(L);
        return 1;
    }

    json_to_lua(L, root, 0);
    cJSON_Delete(root);
    return 1;
}

static cJSON *lua_to_json(lua_State *L, int idx, int depth)
{
    /* 表格分支会在栈上 push / pop，相对索引会跟着漂；先固定成绝对索引 */
    idx = lua_absindex(L, idx);

    if (depth > SCRIPT_JSON_DEPTH_MAX) return cJSON_CreateNull();

    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, idx));
    case LUA_TNUMBER:
        return cJSON_CreateNumber(lua_tonumber(L, idx));
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, idx));
    case LUA_TTABLE: {
        const lua_Integer n = (lua_Integer)lua_rawlen(L, idx);
        const bool array = (n > 0);

        cJSON *node = array ? cJSON_CreateArray() : cJSON_CreateObject();
        if (node == NULL) return NULL;

        if (array) {
            for (lua_Integer i = 1; i <= n; i++) {
                lua_rawgeti(L, idx, (int)i);
                cJSON *child = lua_to_json(L, -1, depth + 1);
                lua_pop(L, 1);
                if (child != NULL) cJSON_AddItemToArray(node, child);
            }
        } else {
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {       /* 非字符串键跳过 */
                    cJSON *child = lua_to_json(L, -1, depth + 1);
                    if (child != NULL) {
                        cJSON_AddItemToObject(node, lua_tostring(L, -2), child);
                    }
                }
                lua_pop(L, 1);
            }
        }
        return node;
    }
    default:
        return cJSON_CreateNull();
    }
}

/* json.encode(table)：失败返回 nil */
static int l_json_encode(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    cJSON *root = lua_to_json(L, 1, 0);
    if (root == NULL) {
        lua_pushnil(L);
        return 1;
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, out);
    cJSON_free(out);
    return 1;
}

static const luaL_Reg json_lib[] = {
    { "decode", l_json_decode },
    { "encode", l_json_encode },
    { NULL, NULL },
};

static int luaopen_json(lua_State *L)
{
    luaL_newlib(L, json_lib);
    return 1;
}

/* ------------------------------ 绑定：web（局域网网页） ------------------------------
 *
 * 让脚本自己在 Web 控制台上开网页：路由挂在 /s 底下（浏览器访问
 * http://<设备 IP>/s/...），只在脚本运行期间有效 —— 离开脚本页脚本就停，网页也就没了。
 *
 * 线程模型：请求在 esp_http_server 的任务里进来，而 Lua 状态只在脚本任务里跑（Lua 不是
 * 线程安全的）。所以处理回调（svc_web_set_custom_handler 注册的那个）在 HTTP 任务里把
 * 请求拷进静态区、置位 s_web_waiting，然后等信号量；脚本任务在自己的循环里看到标志就去
 * 调 Lua 回调、把响应写进响应缓冲、放信号量。回调要快：它慢多久，整个控制台就等多久。
 */

#define SCRIPT_WEB_ROUTE_MAX  8
#define SCRIPT_WEB_RESP_MAX   (24 * 1024)   /* 响应体上限（PSRAM，按需分配） */
#define SCRIPT_WEB_WAIT_MS    2000          /* HTTP 任务等脚本应答的上限 */

typedef struct {
    char path[64];
    char method[8];
    int ref;
} script_web_route_t;

static script_web_route_t s_web_routes[SCRIPT_WEB_ROUTE_MAX];
static int s_web_route_count = 0;

static volatile bool s_web_waiting = false;
static volatile uint32_t s_web_req_id = 0;      /* 每来一个请求 +1 */
static uint32_t s_web_done_id = 0;              /* 脚本刚应答的是哪一个 */
static SemaphoreHandle_t s_web_done = NULL;

static char s_web_method[8];
static char s_web_path[128];
static char s_web_query[256];
static char s_web_body[1024];

static char *s_web_resp = NULL;
static size_t s_web_resp_len = 0;
static char s_web_resp_type[64];
static int s_web_resp_status = 200;

/* %XX 解码（'+' 当普通字符） */
static void web_url_decode(const char *in, char *out, size_t len)
{
    size_t o = 0;

    for (size_t i = 0; in[i] != '\0' && o + 1 < len; i++) {
        if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            const char hex[3] = { in[i + 1], in[i + 2], '\0' };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }

    out[o] = '\0';
}

/* 把响应体放进缓冲（首次用到才分配） */
static void web_resp_set(const void *data, size_t len)
{
    if (s_web_resp == NULL) {
        s_web_resp = malloc(SCRIPT_WEB_RESP_MAX);
        if (s_web_resp == NULL) {
            s_web_resp_len = 0;
            return;
        }
    }

    s_web_resp_len = (len < SCRIPT_WEB_RESP_MAX) ? len : SCRIPT_WEB_RESP_MAX;
    if (s_web_resp_len > 0) memcpy(s_web_resp, data, s_web_resp_len);
}

static void web_resp_text(const char *text)
{
    web_resp_set(text, strlen(text));
}

/* query 串 → Lua 表：k=v&k2=v2（键值都做 URL 解码） */
static void web_push_query(lua_State *L, const char *query)
{
    char buf[sizeof(s_web_query)];
    snprintf(buf, sizeof(buf), "%s", query);

    lua_newtable(L);

    char *save = NULL;
    for (char *pair = strtok_r(buf, "&", &save); pair != NULL;
         pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (eq != NULL) *eq = '\0';
        const char *val = (eq != NULL) ? (eq + 1) : "";

        char key[64];
        char value[256];
        web_url_decode(pair, key, sizeof(key));
        web_url_decode(val, value, sizeof(value));
        if (key[0] == '\0') continue;

        lua_pushstring(L, value);
        lua_setfield(L, -2, key);
    }
}

/* 在脚本任务里处理一个请求（HTTP 任务正在等信号量） */
static void script_web_serve(void)
{
    lua_State *L = s_L;
    script_web_route_t *route = NULL;
    const uint32_t my_id = s_web_req_id;

    s_web_resp_status = 200;
    snprintf(s_web_resp_type, sizeof(s_web_resp_type), "text/plain; charset=utf-8");
    s_web_resp_len = 0;

    if (L != NULL) {
        for (int i = 0; i < s_web_route_count; i++) {
            if (strcmp(s_web_routes[i].method, s_web_method) == 0 &&
                strcmp(s_web_routes[i].path, s_web_path) == 0) {
                route = &s_web_routes[i];
                break;
            }
        }
    }

    if (route == NULL) {
        s_web_resp_status = 404;
        web_resp_text("脚本没有注册这个路径");
        goto done;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, route->ref);

    lua_createtable(L, 0, 5);
    lua_pushstring(L, s_web_method);
    lua_setfield(L, -2, "method");
    lua_pushstring(L, s_web_path);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, s_web_query);
    lua_setfield(L, -2, "query_string");
    lua_pushstring(L, s_web_body);
    lua_setfield(L, -2, "body");
    web_push_query(L, s_web_query);
    lua_setfield(L, -2, "query");

    if (lua_pcall(L, 1, 3, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        const bool exiting = (lua_touserdata(L, -1) == (void *)&s_exit_marker);
        char buf[FW_SCRIPT_ERR_MAX];

        snprintf(buf, sizeof(buf), "%s", (msg != NULL) ? msg : "脚本回调出错");
        lua_pop(L, 1);

        if (exiting) {
            s_web_resp_status = 503;
            web_resp_text("脚本已退出");
        } else {
            script_error(buf, true);            /* 收口：记错误 + 串口 + 通知脚本管理 */
            s_web_resp_status = 500;
            web_resp_text(buf);
        }
        goto done;
    }

    /* 返回值：(content_type, body) 或 (status, content_type, body) */
    int ct_idx = -3;
    int body_idx = -2;

    if (lua_isnumber(L, -3)) {
        s_web_resp_status = (int)lua_tointeger(L, -3);
        ct_idx = -2;
        body_idx = -1;
    }

    if (lua_type(L, ct_idx) == LUA_TSTRING) {
        snprintf(s_web_resp_type, sizeof(s_web_resp_type), "%s", lua_tostring(L, ct_idx));
    }

    if (lua_type(L, body_idx) == LUA_TSTRING) {
        size_t n = 0;
        const char *body = lua_tolstring(L, body_idx, &n);
        web_resp_set(body, n);
    } else if (lua_type(L, ct_idx) == LUA_TNIL) {
        s_web_resp_status = 500;
        web_resp_text("回调要返回 (content_type, body)");
    }

    lua_pop(L, 3);

done:
    /* 只清"我这一次"的等待标志：应答期间若来了新请求，别把它的标志一起清掉 */
    if (s_web_req_id == my_id) s_web_waiting = false;
    s_web_done_id = my_id;
    if (s_web_done != NULL) xSemaphoreGive(s_web_done);
}

/* HTTP 任务侧：把请求交给脚本任务并等它应答 */
static esp_err_t web_route_cb(const svc_web_req_t *req, svc_web_resp_t *resp, void *user)
{
    (void)user;

    if (!s_running || s_L == NULL || s_web_route_count == 0 || s_web_done == NULL) {
        return ESP_ERR_NOT_FOUND;               /* 没有脚本在提供网页 */
    }

    const int64_t t0 = esp_timer_get_time();

    snprintf(s_web_method, sizeof(s_web_method), "%s", req->method);
    snprintf(s_web_path, sizeof(s_web_path), "%s", req->path);
    snprintf(s_web_query, sizeof(s_web_query), "%s", req->query);
    snprintf(s_web_body, sizeof(s_web_body), "%s", req->body);

    const uint32_t id = ++s_web_req_id;
    s_web_waiting = true;

    /* 等应答：只收"这一次请求"的应答 —— 上一次超时留下的 token 要丢掉，
     * 否则会把上一页的内容发给这一次 */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SCRIPT_WEB_WAIT_MS);
    bool served = false;

    while (!served) {
        TickType_t left = deadline - xTaskGetTickCount();
        if ((int32_t)left <= 0) break;

        if (xSemaphoreTake(s_web_done, (left > pdMS_TO_TICKS(50)) ? pdMS_TO_TICKS(50) : left)
            == pdTRUE) {
            served = (s_web_done_id == id);
        }
    }

    if (!served) {
        s_web_waiting = false;
        ESP_LOGW(TAG, "web request timeout: %s %s", req->method, req->path);
        return ESP_ERR_TIMEOUT;
    }

    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (ms > 500) {
        ESP_LOGW(TAG, "web %s %s took %lld ms（回调要快，别在里面做重活）",
                 req->method, req->path, (long long)ms);
    }

    resp->status = s_web_resp_status;
    resp->content_type = s_web_resp_type;
    resp->body = s_web_resp;
    resp->body_len = s_web_resp_len;
    return ESP_OK;
}

/* 脚本停止时清掉它注册的路由 */
static void web_routes_reset(void)
{
    if (s_L != NULL) {
        for (int i = 0; i < s_web_route_count; i++) {
            if (s_web_routes[i].ref != LUA_NOREF) {
                luaL_unref(s_L, LUA_REGISTRYINDEX, s_web_routes[i].ref);
                s_web_routes[i].ref = LUA_NOREF;
            }
        }
    }
    s_web_route_count = 0;
}

static int web_route_add(lua_State *L, const char *method)
{
    const char *path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (path[0] != '/' || strlen(path) >= sizeof(s_web_routes[0].path)) {
        return luaL_error(L, "路径要写成 \"/xxx\"（%d 字节以内）",
                          (int)sizeof(s_web_routes[0].path) - 1);
    }

    /* 同一路径 + 方法重复注册就替换 */
    for (int i = 0; i < s_web_route_count; i++) {
        if (strcmp(s_web_routes[i].path, path) == 0 &&
            strcmp(s_web_routes[i].method, method) == 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_web_routes[i].ref);
            lua_pushvalue(L, 2);
            s_web_routes[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
            return 0;
        }
    }

    if (s_web_route_count >= SCRIPT_WEB_ROUTE_MAX) {
        return luaL_error(L, "路由太多了（上限 %d）", SCRIPT_WEB_ROUTE_MAX);
    }

    script_web_route_t *r = &s_web_routes[s_web_route_count++];
    snprintf(r->path, sizeof(r->path), "%s", path);
    snprintf(r->method, sizeof(r->method), "%s", method);
    lua_pushvalue(L, 2);
    r->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_web_get(lua_State *L)
{
    return web_route_add(L, "GET");
}

static int l_web_post(lua_State *L)
{
    return web_route_add(L, "POST");
}

/* 网页地址；Web 控制台没开（没连上 Wi-Fi）时返回 nil */
static int l_web_url(lua_State *L)
{
    svc_net_status_t st;

    if (!svc_web_is_running() || svc_net_get_status(&st) != ESP_OK || !st.wifi_connected) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushfstring(L, "http://%s/s", st.ip_addr);
    return 1;
}

static int l_web_ready(lua_State *L)
{
    lua_pushboolean(L, svc_web_is_running());
    return 1;
}

static const luaL_Reg web_lib[] = {
    { "get", l_web_get },
    { "post", l_web_post },
    { "url", l_web_url },
    { "ready", l_web_ready },
    { NULL, NULL },
};

static int luaopen_web(lua_State *L)
{
    luaL_newlib(L, web_lib);
    return 1;
}

/* ------------------------------ 能力裁剪 -------------------------------
 *
 * 脚本头部可以写一行注释 -- @perm io,file,net 声明要用的模块；写了声明就
 * 只注册列出的模块，未声明的模块在脚本里是 nil、调用即 Lua 报错。不写声明的
 * 脚本（含内置示例）保持"全部模块可用"。
 *
 * ui / sys / timer 标记为基础能力，无论声明如何都注册：脚本的界面、时间
 * 与定时器是它自己"能跑起来并报错"的最小前提，否则连 ui.toast("脚本出错")
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
    { "util",     luaopen_util,     0 },
    { "json",     luaopen_json,     0 },
    { "web",      luaopen_web,      0 },
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
            ESP_LOGD(TAG, "module not permitted: %s", SCRIPT_MODULES[i].name);
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

/* 脚本出错统一入口：记下描述（供脚本管理显示）、串口一条、必要时提示用户、通知脚本管理。
 * 同一个错误反复出现时不再弹 Toast（坏掉的定时器每秒报一次会把提示刷满），但每次都
 * 会发 SCRIPT_FAILED —— App 那边按"错误文本变了才弹对话框"自己收口。 */
static char s_notified_error[FW_SCRIPT_ERR_MAX] = { 0 };

static void script_error(const char *msg, bool notify)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s",
             (msg != NULL && msg[0] != '\0') ? msg : "脚本出错");
    ESP_LOGE(TAG, "script error: %s", s_last_error);

    if (notify && strcmp(s_notified_error, s_last_error) != 0) {
        strlcpy(s_notified_error, s_last_error, sizeof(s_notified_error));
        toast("脚本出错");
    }

    svc_event_bus_publish(SVC_EVENT_SCRIPT_FAILED, NULL, 0);
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
     * 置空，之后任何 lv_screen_load* 都会空指针崩溃。
     *
     * 只有脚本页确实在前台时才切：用户可能在脚本运行中按了状态栏返回键，那时前台
     * 已经是别的屏，硬切回 s_prev_screen 反而会抢回前台；此时直接删页即可。
     * fw_window 的 s_active 是裸指针，先 sync 再和 lv_screen_active() 一起核对。 */
    fw_window_sync_active();
    if (s_page != NULL && s_prev_screen != NULL && lv_screen_active() == s_page) {
        /* s_prev_screen 是活的：它只会在 s_page 不在前台时被重记（见 l_ui_page），
         * 而重记与切屏在同一次加锁内完成，中间不会有主题重建把目标屏删掉。 */
        fw_window_switch_to(s_prev_screen, LV_SCREEN_LOAD_ANIM_NONE, 0);
    }
    s_prev_screen = NULL;

    /* 「运行日志」页的刷新定时器与缓冲随脚本一起收掉（页面本身挂在 s_page 下） */
    script_log_page_close();

    /* 脚本建的界面对象随页面根一起删掉 */
    if (s_page != NULL) {
        lv_obj_delete(s_page);
        s_page = NULL;
    }
    widgets_reset();
    lvgl_port_unlock();

    /* 先退订：否则事件回调会打到马上要关掉的 Lua 状态上 */
    for (int i = 0; i < SCRIPT_EVENT_MAX; i++) {
        if (s_subs[i].ref != LUA_NOREF) {
            svc_event_bus_unsubscribe(s_subs[i].id, bus_event_cb);
        }
    }
    subs_reset();

    external_reset();               /* 退订 MQTT 主题 + 断开 MQTT / WebSocket */
    gatt_reset();                   /* 清通知回调、必要时断开脚本发起的中心连接 */
    web_routes_reset();             /* 它注册的网页路由随脚本一起失效 */
    flush_callback_queues();        /* 清掉回调队列里属于本状态的引用 */

    if (s_L != NULL) {
        lua_close(s_L);
        s_L = NULL;
    }
    timers_reset();
    flush_callback_queues();        /* 关闭过程中可能又有回调投递进来，再清一次 */

    /* 有网页请求正等着应答（HTTP 任务在等信号量）：给一句"脚本已停止"，别让它干等到超时 */
    if (s_web_waiting) {
        s_web_resp_status = 503;
        snprintf(s_web_resp_type, sizeof(s_web_resp_type), "text/plain; charset=utf-8");
        web_resp_text("脚本已停止");
        s_web_done_id = s_web_req_id;
        s_web_waiting = false;
        if (s_web_done != NULL) xSemaphoreGive(s_web_done);
    }
}

static void start_script(void)
{
    /* 上一次异常退出可能留下没取走的事件，先丢掉 */
    flush_callback_queues();

    script_log_reset();

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        script_error("内存不足，Lua 状态没建起来", false);
        toast("内存不足，脚本未启动");
        s_starting = false;     /* 不清会把整个脚本系统永久锁住 */
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

    const int loaded = luaL_loadfile(L, s_pending_path);
    if (loaded != LUA_OK) {
        script_error(lua_tostring(L, -1), true);
        teardown_script();
        s_current[0] = '\0';
        s_starting = false;
        return;
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        /* 主流程里调 sys.exit()：不是错误，按正常退出收尾 */
        if (lua_touserdata(L, -1) == (void *)&s_exit_marker) {
            lua_pop(L, 1);
            toast("脚本已退出");
            teardown_script();      /* 脚本可能已经建了界面 / 订阅，按停止处理 */
            s_current[0] = '\0';
            s_last_error[0] = '\0';
            s_starting = false;
            svc_event_bus_publish(SVC_EVENT_SCRIPT_STOPPED, NULL, 0);
            return;
        }

        script_error(lua_tostring(L, -1), true);
        teardown_script();          /* 脚本可能已经建了界面 / 订阅，按停止处理 */
        s_current[0] = '\0';
        s_starting = false;
        return;
    }

    s_last_error[0] = '\0';         /* 跑起来了：清掉上一次的错误 */
    s_notified_error[0] = '\0';
    s_running = true;
    s_starting = false;
    toast("脚本已启动");

    /* 没建界面的脚本给一个「运行日志」页：这样有无界面都一样 —— 脚本有自己的页面，
     * 离开它就停脚本（fw_script_owns_screen 的判断） */
    script_log_page_open();

    svc_event_bus_publish(SVC_EVENT_SCRIPT_STARTED, NULL, 0);
    ESP_LOGI(TAG, "running: %s", s_pending_path);
}

static void stop_script(void)
{
    teardown_script();

    s_running = false;
    s_current[0] = '\0';
    s_last_error[0] = '\0';     /* 正常停止不算错误 */

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
            drain_gatt_notifications();
            run_due_timers();
            if (s_web_waiting) script_web_serve();      /* 网页请求：HTTP 任务正等着 */
        }
    }
}

/* 把内置示例释放到脚本目录：同名覆盖，跟随固件更新。内容都是构建期嵌进来的资源
 * （见文件头的 extern 声明与 CMakeLists 的 EMBED_FILES），这里只列"卡上的名字 + 资源起止"。
 * 写文件要 TF 卡，/sdcard 没挂载时只告警，不影响启动；目录不存在时先建（已存在会失败，忽略）。 */
static void release_samples(void)
{
    static const struct {
        const char *name;               /* 释放到卡上的名字 */
        const char *begin;
        const char *end;
    } samples[] = {
        { "counter.lua",        counter_lua_start,       counter_lua_end },
        { "clock.lua",          clock_lua_start,         clock_lua_end },
        { "snake.lua",          snake_lua_start,         snake_lua_end },
        { "io.lua",             io_lua_start,            io_lua_end },
        { "gomoku.lua",         gomoku_lua_start,        gomoku_lua_end },
        { "脚本接口参考.txt",    api_reference_txt_start, api_reference_txt_end },
    };

    svc_storage_mkdir(FW_SCRIPT_DIR);

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        char path[FW_SCRIPT_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", FW_SCRIPT_DIR, samples[i].name);

        const size_t len = (size_t)(samples[i].end - samples[i].begin);
        if (svc_storage_write(path, samples[i].begin, len) != ESP_OK) {
            ESP_LOGW(TAG, "release sample failed (no TF card?): %s", path);
        } else {
            ESP_LOGI(TAG, "sample released: %s", path);
        }
    }
}

esp_err_t fw_script_init(void)
{
    if (s_task != NULL) return ESP_OK;

    ESP_LOGI(TAG, "init: internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    timers_reset();

    s_queue = xQueueCreate(4, sizeof(script_cmd_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    s_cb_queue = xQueueCreate(SCRIPT_CB_QUEUE_LEN, sizeof(int));
    if (s_cb_queue == NULL) return ESP_ERR_NO_MEM;

    s_bus_queue = xQueueCreate(SCRIPT_EVENT_MAX, sizeof(script_bus_evt_t));
    if (s_bus_queue == NULL) return ESP_ERR_NO_MEM;

    s_msg_queue = xQueueCreate(SCRIPT_MSG_QUEUE_LEN, sizeof(script_msg_evt_t));
    if (s_msg_queue == NULL) return ESP_ERR_NO_MEM;

    s_gatt_sem = xSemaphoreCreateBinary();
    if (s_gatt_sem == NULL) return ESP_ERR_NO_MEM;

    s_gatt_queue = xQueueCreate(SCRIPT_GATT_NOTIFY_QUEUE_LEN, sizeof(script_gatt_msg_t));
    if (s_gatt_queue == NULL) return ESP_ERR_NO_MEM;

    /* 脚本网页：HTTP 任务与脚本任务之间的应答信号（深度 1 够了，HTTP 任务同时只处理一个请求） */
    if (s_web_done == NULL) s_web_done = xSemaphoreCreateBinary();
    if (s_web_done == NULL) return ESP_ERR_NO_MEM;

    /* 中心角色的异步结果与通知统一走这个回调（App 那边另占一个回调槽） */
    svc_bt_central_register_cb(gatt_event_cb, NULL);

    /* 把脚本网页区（/s 前缀）挂到 Web 控制台上；没连 Wi-Fi 时控制台没监听，注册也不报错 */
    if (svc_web_set_custom_handler(web_route_cb, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "web console handler busy, script web pages unavailable");
    }

    subs_reset();

    /* 栈放 PSRAM：脚本任务要 8 KB，内部 DRAM 在 Wi-Fi / BLE 起来后很紧，
     * 留着给相机（DVP 要一整块 7.6 KB 连续内部 DMA）等真正的内部内存用户。
     * 脚本跑在 PSRAM 栈上没问题，它不做 flash 擦写期间的深调用。 */
    if (xTaskCreatePinnedToCoreWithCaps(script_task, "script_task", SCRIPT_TASK_STACK, NULL,
                                        SCRIPT_TASK_PRIO, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "script task create failed: internal largest=%u free=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    release_samples();

    ESP_LOGI(TAG, "initialized (scripts in %s)", FW_SCRIPT_DIR);
    return ESP_OK;
}

/* ---------------------------- 扫描 / 元信息 ---------------------------- */

/* 脚本不强制放在 scripts 目录：TF 卡与内置存储整盘都扫（scripts 只是内置示例的落地目录）。
 * 递归深度与路径长度都有上限，超出的跳过，避免出现半截路径。 */
#define SCRIPT_SCAN_DEPTH_MAX   6

static const char *const SCRIPT_SCAN_ROOTS[] = { "/sdcard", "/internal" };

/* 扩展名判断不分大小写：.lua / .LUA 都算脚本（下载来的文件名大小写不定） */
static bool script_has_lua_ext(const char *name)
{
    const size_t n = strlen(name);
    if (n < 4) return false;

    const char *p = name + n - 4;
    return p[0] == '.' && (p[1] == 'l' || p[1] == 'L') && (p[2] == 'u' || p[2] == 'U')
        && (p[3] == 'a' || p[3] == 'A');
}

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

/* 递归扫一个目录；depth 超过上限就不再往下（避免深层嵌套把调用者栈压爆） */
static void script_scan_dir(const char *dir, int depth, fw_script_info_t *out, size_t max,
                            size_t *count)
{
    if (depth > SCRIPT_SCAN_DEPTH_MAX || *count >= max) return;

    svc_storage_iter_t it = NULL;
    if (svc_storage_iter_start(dir, &it) != ESP_OK) return;   /* 没挂载 / 打不开：跳过 */

    svc_storage_entry_t *e = NULL;
    while (*count < max && (e = svc_storage_iter_next(it)) != NULL) {
        char path[FW_SCRIPT_PATH_MAX];

        if (snprintf(path, sizeof(path), "%s/%s", dir, e->name) >= (int)sizeof(path)) {
            ESP_LOGW(TAG, "skip (path too long): %s/%s", dir, e->name);
            continue;
        }

        if (e->is_dir) {
            script_scan_dir(path, depth + 1, out, max, count);
            continue;
        }
        if (!script_has_lua_ext(path)) continue;

        fw_script_info_t *info = &out[*count];
        snprintf(info->path, sizeof(info->path), "%s", path);
        read_meta(info);
        (*count)++;
    }

    svc_storage_iter_end(it);
}

esp_err_t fw_script_scan(fw_script_info_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL) return ESP_ERR_INVALID_ARG;

    *count = 0;
    for (size_t i = 0; i < sizeof(SCRIPT_SCAN_ROOTS) / sizeof(SCRIPT_SCAN_ROOTS[0]); i++) {
        if (*count >= max) break;
        script_scan_dir(SCRIPT_SCAN_ROOTS[i], 0, out, max, count);
    }
    return ESP_OK;
}

/* -------------------------------- 运行 / 停止 -------------------------------- */

esp_err_t fw_script_run(const char *path)
{
    if (path == NULL) return ESP_ERR_INVALID_ARG;
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (s_running || s_starting) return ESP_ERR_INVALID_STATE;

    snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
    name_of(s_pending_path, s_current, sizeof(s_current));

    /* 先置位再投递：从投递到任务真正开始跑的这段时间里也要挡住第二次调用，
     * 否则 s_pending_path 会被后一次覆盖，最后跑起来的是后一个脚本 */
    s_starting = true;

    const script_cmd_t cmd = CMD_RUN;
    if (xQueueSend(s_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_starting = false;
        return ESP_FAIL;
    }
    return ESP_OK;
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

bool fw_script_owns_screen(void)
{
    lvgl_port_lock(0);
    const bool owns = (s_page != NULL) && (lv_screen_active() == s_page);
    lvgl_port_unlock();
    return owns;
}

const char *fw_script_last_error(void)
{
    return (s_last_error[0] != '\0') ? s_last_error : NULL;
}
