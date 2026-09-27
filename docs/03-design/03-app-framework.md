# 应用与脚本框架详细设计

系统提供两套应用模型：**原生 App**（C 编写，编译进固件，由 `fw_app_mgr` 管理）与**脚本**（Lua 编写，放 TF 卡，由 `fw_script` 管理）。本文档定义两者的编程模型与约定。

## 1. 应用模型对比

| 维度 | 原生 App | 脚本 |
|------|----------|------|
| 语言 | C | Lua 5.5 |
| 存放 | 编译进固件（`main/apps/`） | TF 卡脚本目录 |
| 注册 | `fw_app_mgr_register()` | 脚本管理 App 扫描目录 |
| 生命周期 | create / start / pause / resume / destroy / back | 加载 / 执行 / 停止 |
| 入口 | `fw_app_mgr_launch("Name")` | `fw_script_run(path)` |
| 能力 | Framework + Services | 通过绑定访问 Framework / Services / `svc_io` |

两者共用事件总线、配置系统与存储；桌面只展示原生 App，脚本统一在脚本管理 App 里运行。

## 2. 原生 App 模板

### 2.1 目录结构

每个 App 一个独立子目录：

```
main/apps/app_clock/
├── app_clock.h           # App 头文件（可选）
├── app_clock.c           # App 主体
└── assets/               # App 私有资源（可选）
```

桌面图标不放在 App 目录里：22 个图标集中在 `main/framework/assets/fw_icons.c` 的
`icon_home_*`（由 `tools/gen_fw_icons.py` 生成），App 用描述符的 `icon` 引用。

新增 App 时把目录加进 `main/apps/CMakeLists.txt` 的 `SRC_DIRS` / `INCLUDE_DIRS`。

### 2.2 最小 App 示例

```c
// main/apps/app_clock/app_clock.c
#include "app_common.h"
#include "fw_common.h"
#include "esp_lvgl_port.h"

static lv_obj_t *s_root;
static lv_timer_t *s_timer;

static void refresh_cb(lv_timer_t *t) {
    // 更新时间显示
}

static void *on_create(void)
{
    lvgl_port_lock(0);
    lv_obj_t *content;
    s_root = fw_ui_page(&content);
    lv_timer_create(refresh_cb, 1000, NULL);
    lvgl_port_unlock();
    return s_root;
}

static void on_start(void *ctx) { /* 订阅事件 */ }
static void on_pause(void *ctx) { /* 退订事件 */ }
static void on_destroy(void *ctx)
{
    lvgl_port_lock(0);
    if (s_timer) lv_timer_delete(s_timer);
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_clock_desc = {
    .name = "Clock",
    .title = "时钟",
    .icon = &icon_home_clock,
    .on_create = on_create,
    .on_start = on_start,
    .on_pause = on_pause,
    .on_destroy = on_destroy,
};
```

描述符字段：`name` 是内部标识（英文，启动 / 注册用），`title` 是界面显示名（中文，
桌面网格用），`icon` 是桌面彩色图标。

`on_back(ctx)` 为可选字段：返回 `true` 表示"返回"已在 App 内处理（例如回到上一级页面），`fw_app_mgr` 不再退出到上一级；返回 `false` 或未提供 `on_back` 则退出到上一级（通常是桌面）。状态栏返回键与 BOOT 单击都走这条路径，App 不要自己做返回按钮。

## 3. App 生命周期详解

### 3.1 状态机

```
                  on_create
       (none) ───────────────► Created
                                  │
                                on_start
                                  ▼
                              Active ─────────────┐
                                  │               │
                              on_pause         (user input)
                                  ▼               │
                              Paused ─────────────┘
                                  │ on_resume
                                  ▼
                              Active
                                  │ back (user)
                                  ▼
                              on_destroy
                                  ▼
                              (destroyed)
```

### 3.2 各阶段职责

| 阶段 | App 应该做 | App 不应该做 |
|------|-----------|--------------|
| `on_create` | 创建所有 LVGL 对象，分配资源 | 调用阻塞 API，订阅事件 |
| `on_start` | 订阅事件，启动计时器 | 创建对象（应在 create 完成） |
| `on_pause` | 取消订阅，暂停计时器，保存关键状态 | 删除 LVGL 对象 |
| `on_resume` | 重新订阅，重启计时器，恢复状态 | 重新创建 LVGL 对象 |
| `on_destroy` | 释放所有资源（LVGL 对象、互斥锁、任务、订阅） | 调用阻塞 API |

### 3.3 重要约束

- **`on_create` 必须快速**（< 100 ms），避免拖慢桌面启动
- **换主题只重建当前前台 App**（其余 App 标记待重建、下次显示前重建）：`on_create` 里的副作用（网络请求、写 NVS 等）不要依赖"切主题时立刻跑一遍"，需要"每次进前台都刷新"就写在 `on_start` / `on_resume` 里
- **LVGL 操作必须在 `lvgl_port_lock(0) / unlock()` 之间**
- **资源分配失败必须优雅降级**：PSRAM 不够时显示错误提示而非崩溃
- **所有定时器必须在 `on_destroy` 删除**，否则下一次启动会泄漏
- **不要在 App 里加大块 static 缓冲**（内部 RAM 只有十几 KB 余量）

## 4. App 与 Services 交互

### 4.1 播放音乐

```c
static void btn_play_cb(lv_event_t *e)
{
    svc_audio_source_t src = {
        .type = SVC_AUDIO_SRC_FILE,
        .uri = "/sdcard/music/test.mp3",
    };
    if (svc_audio_play(&src) != ESP_OK) {
        fw_ui_toast("播放失败", 2000);
    }
}
```

### 4.2 订阅事件

```c
static void on_evt(const svc_event_t *evt, void *user)
{
    if (evt->id == SVC_EVENT_WIFI_CONNECTED) {
        lvgl_port_lock(0);
        lv_label_set_text(s_wifi_label, "已连接");
        lvgl_port_unlock();
    }
}

// on_start 中订阅
svc_event_bus_subscribe(SVC_EVENT_WIFI_CONNECTED, on_evt, NULL);

// on_pause / on_destroy 中取消
svc_event_bus_unsubscribe(SVC_EVENT_WIFI_CONNECTED, on_evt);
```

### 4.3 持久化设置

```c
// 读取
uint32_t volume;
svc_settings_get_u32("app_music", "volume", &volume, 80);  // 默认 80

// 写入
svc_settings_set_u32("app_music", "volume", new_volume);
```

### 4.4 提示音

```c
svc_audio_play_tone_async(1000, 100);   // 1 kHz, 100 ms
```

## 5. App 内部状态管理

App 用私有 ctx 结构体保存状态，`on_create` 返回根屏对象，作为各生命周期回调的 ctx：

```c
typedef struct {
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_timer_t *timer;
    bool is_24h;
} clock_ctx_t;

static clock_ctx_t s_ctx = {0};

static void *clock_on_create(void)
{
    lvgl_port_lock(0);
    lv_obj_t *content;
    s_ctx.root = fw_ui_page(&content);
    s_ctx.timer = lv_timer_create(clock_refresh, 1000, NULL);
    lvgl_port_unlock();
    return s_ctx.root;
}
```

### 5.1 多页面 App：容器显隐 + on_back

App 只有一块根屏（`fw_window` 只认它），所以 App 内部的多级页面要在根屏里建几块同尺寸容器，
用 `lv_obj_set_hidden()` 切换，返回键交给 `on_back` 处理（时钟 App 的主页 / 设置页 / 时区页就是这么做的）：

```c
static bool clock_on_back(void *ctx)
{
    if (s_page_cur == PAGE_MAIN) return false;   /* 主页：返回 false 交给框架退出 App */
    show_page(PAGE_MAIN);
    return true;                                 /* 子页面：已处理，留在 App 内 */
}
```

- 容器切换只改显隐、不删对象，所以在按钮回调里切换页面是安全的（不会删掉"正在处理事件的控件"）。
- 需要重建的内容（如时钟 App 的时区网格）才在进入页面时 `lv_obj_clean()` 重建；重建的是容器里的子对象，不是按下事件所在的控件。
- 长列表直接放进页面里那块可滚动容器，不要再套一层可滚动容器：两层都能滚时，拖动先落到内层，用户不知道滚的是哪一个。
- "每秒刷新"的界面用固定周期轮询 + 判断要显示的值有没有变（时钟 App 是 100 ms 轮询、对比秒数），不要用 `lv_timer_set_period()` 去对齐整秒相位：它会改写定时器的 `last_run`，行为依赖 LVGL 内部实现，出问题很难查。

## 6. App 注册机制

`main/apps/src/app_common.c` 集中注册：

```c
extern const fw_app_desc_t app_clock_desc;
extern const fw_app_desc_t app_music_desc;
// ...

void app_register_all(void)
{
    fw_app_mgr_register(&app_home_desc);
    fw_app_mgr_register(&app_clock_desc);
    fw_app_mgr_register(&app_music_desc);
    // ...
}
```

注册顺序即桌面格子顺序（Home 之外），必须与原始需求「内置应用」列表一致；
新增 App 时插到对应位置，不要只追加到末尾。

## 7. App 之间通信

### 7.1 事件总线

```c
// App A 发送
svc_event_bus_publish(SVC_EVENT_USER_BASE, &my_data, sizeof(my_data_t));

// App B 接收
svc_event_bus_subscribe(SVC_EVENT_USER_BASE, on_user_evt, NULL);
```

### 7.2 URI 启动

```c
fw_app_mgr_launch("Music");
fw_app_mgr_launch_with_args("File", "path=/sdcard/music");
fw_app_mgr_launch_uri("Music?song=1");
```

App 用 `fw_app_mgr_get_args()` 读取启动参数。

**约定：内容类 App 用 `App?path=<绝对路径>` 从文件管理直接打开文件**（`Music` / `Image` / `Editor` 三个都支持）。读参数放两处：`on_create`（首次创建）与 `on_resume`（重新进前台）。只放在 `on_start` 会漏 —— 从返回栈回来的那条路径**只发 `on_resume`**（`fw_app_mgr` 的 `app_launch()` 分成 "在返回栈里" 与 "已创建但不在栈里" 两种），而首次创建时 `on_resume` 又不发，所以两处都要有；`on_start` 只承担恢复刷新定时器这类事。两处都读还有个好处：反复从文件管理点同一个文件，每次都会重新打开。`launch_with_args` / `launch_uri` 每次启动都会重写参数，普通 `launch()` 会把参数清空，所以不带参数进前台不会误触发。

## 8. App 调试

```c
static const char *TAG = "app.clock";
ESP_LOGI(TAG, "Clock created");
ESP_LOGE(TAG, "Failed to parse: %s", input);
```

性能 / 内存 / 日志由「性能监控」「系统日志」两个 App 展示（数据来自 `svc_sysinfo`）。

## 9. App 开发最佳实践

1. **永远在 LVGL 操作前后加锁**
2. **永远在 on_destroy 释放所有资源**（LVGL 对象、定时器、订阅、互斥锁）
3. **避免在 on_create 中做耗时操作**
4. **使用 ctx 结构体而非全局变量**
5. **配置项用 svc_settings，命名空间用 App 名（如 App 天气用 `weather`）**
6. **错误必须优雅处理**（不崩溃，用 `fw_ui_toast()` 提示）
7. **资源路径用绝对路径**：`/sdcard/...` 或 `/internal/...`
8. **图标与显示名走描述符**：`icon` 引用 `main/framework/assets/fw_icons.c` 里的彩色图标（`icon_home_*`），`title` 用原始需求「内置应用」里的中文名称
9. **App 内部子页用显示 / 隐藏切换 + `on_back`**：一个 App 只占一张 LVGL 屏，子页做成根对象里的容器；"返回"统一由状态栏返回键触发

## 10. 脚本模型

### 10.1 存放与元信息

- 脚本不强制放在固定目录：脚本管理递归扫描 TF 卡与内置存储整盘，收录所有 `.lua`（扩展名不分大小写）；`scripts` 目录只是内置示例的落地位置
- 每个脚本可带一段元信息（名称 / 说明 / 入口），没有则用文件名
- 内置示例脚本随固件打包，每次启动释放到脚本目录（同名覆盖，便于随固件更新）
- 下载脚本走「下载」App：输入 `.lua` 链接即可落到脚本目录，脚本管理里不再重复做下载

### 10.2 生命周期

- 同一时刻只运行一个前台脚本：加载 → 执行 → 停止 / 异常退出
- 停止或异常时释放脚本创建的资源（界面对象、定时器、订阅）
- **脚本总有自己的一页**：调了 `ui.page()` 的就是脚本自己建的界面；没调（无界面脚本）时 `fw_script` 给一个「运行日志」页显示它的 `print` 输出（`print` 同时进串口日志，标签 `script`）。有无界面逻辑一致：**离开这一页（返回键 / BOOT 双击 / 主页键 / 换主题）就停脚本**（`fw_app_mgr` 里用 `fw_script_owns_screen()` 判断，脚本停止会把屏还给启动它的 App）
- 脚本也可以自己调 `sys.exit()` 结束（和用户点「停止」同一条收尾路径）
- 脚本异常不影响系统，错误信息（行号 + 描述）写日志并提示：`fw_script_last_error()` 存最近一次错误（成功启动 / 正常停止后清空），同时发 `SVC_EVENT_SCRIPT_FAILED` 事件，脚本管理据此弹对话框
- 脚本的**网页能力**（`web.get` / `web.post`）挂在设备 Web 控制台的 `/s` 前缀下，靠 `svc_web_set_custom_handler()` 接进去。请求在 HTTP 任务里进来，而 Lua 只在脚本任务里跑，中间是一道"HTTP 任务拷贝请求 → 等应答信号量（2 s）→ 脚本任务调 Lua 回调写响应"的桥：**回调必须尽快返回**（它慢多久整个控制台就等多久，超过 500 ms 会打告警），别在回调里做 `net.http_get` 这类阻塞操作。脚本停止时路由一起清空（`web_routes_reset()`），此刻若还有请求挂在那里，直接回 503

### 10.3 运行时

- Lua 5.5 运行时在 `fw_script` 内
- 脚本任务 `script_task`（优先级 4，核心 0，栈 8 KB 放 PSRAM）
- 脚本配置经 `svc_settings` 持久化（命名空间 `script`）
- 数量与规模上限（超了会报 Lua 错或截断，写脚本时留意）：
  - 定时器 16 个、事件订阅 16 个、控件 256 个、画布 8 块（单块 ≤ 320 × 212、宽必须偶数）、网页路由 8 条
  - 单次 Lua 调用 2000 万条指令（防死循环兜底；长计算要拆到多次定时回调里做）
  - I2C 单次读 64 字节；HTTP 响应 8 KB、请求体 2 KB、请求头 512 字节；MQTT / WebSocket 单条消息 192 字节（超出截断）；`file.read` 单文件 1 MB
  - 网页响应体 24 KB（超出截断）；`web` 回调要在 2 s 内返回，否则这次请求按超时失败（HTTP 任务等不起太久）
  - 运行日志环 2 KB（「运行日志」页只显示本次运行的最近输出）

### 10.4 权限与沙箱

- 脚本头部用注释声明要用的能力模块：`-- @perm io,file,net`；模块名与 §11 的绑定表一致，解析范围是文件开头若干行，与元信息解析一致
- 不写声明表示全部模块可用，兼容内置示例脚本
- 写了声明就只注册列出的模块，未声明的模块在脚本里为 nil，调用即报 Lua 错
- `ui` / `sys` / `timer` 是基础能力，无论怎么写声明都会注册：脚本靠它们建界面、取时间，并在出错时给出提示
- 声明里出现不认识的模块名时记一条告警并忽略该名字
- 运行时只打开 base / string / table / math / utf8，不开 io / os / package / debug
- ESP32-S3 无 MMU，这是软件级限制

## 11. 脚本能力绑定

Lua 侧以模块（全局表）的形式暴露能力，模块名与函数清单见 `main/framework/src/fw_script.c`。

| 模块 | 提供的函数 |
|------|-----------|
| `ui` | `page` / `label` / `row` / `image` / `progress` / `canvas` / `input` / `dialog` / `list` / `item` / `button` / `box` / `grid` / `toast`；控件方法 `set_text` / `set_value` / `set_src` / `set` / `get_text` / `set_size`（图片改尺寸即缩放）/ `set_color` / `set_text_color` / `set_radius`，画布另有 `fill` / `rect` / `line` / `circle` |
| `sys` | `uptime` / `now` / `localtime`（表只有 `year` / `month` / `day` / `hour` / `min` / `sec`，**没有 weekday**）/ `mem`（返回内部 SRAM 剩余 / 历史最低与 PSRAM 剩余，单位字节）/ `exit`（脚本自己结束，等价于点「停止」；当前 Lua 调用栈会立刻展开）。**系统级设置（亮度 / 主题 / 时区）不开放给脚本**，避免脚本改到 NVS 里的全局配置 |
| `timer` | `every(ms, fn)` / `after(ms, fn)`（一次性）/ `cancel(id)`（两者都返回 id） |
| `input` | `on_click`（订阅触摸点击，回调收到 `{x, y, pressed}` 表，坐标是屏幕坐标）；BOOT 键事件请用 `event.subscribe("key", fn)` |
| `event` | `subscribe` / `publish`；固定事件名见实现里的 EVENT_MAP（触摸 / BOOT 按键（`key`）/ IMU 姿态与运动 / Wi-Fi / 蓝牙状态 / 亮度 / TF 卡挂载 / 录音结束 / 脚本启停），另有 `user.` 开头的自定义事件名，同一名字稳定映射到同一事件号。`key` 的回调参数是 1 字节字符串，`string.byte(v)` 得事件序号：0 单击 / 1 双击 / 2 长按（熄屏时的第一次按键只用于唤醒，不会送到脚本） |
| `io` | `gpio_write` / `gpio_read` / `pwm_set` / `pwm_stop` / `adc_read` / `i2c_write` / `i2c_read` / `i2c_write_reg` / `i2c_read_reg`（后两个是"先写寄存器号再读写"的常见写法，两次独立传输、没有 repeated start）/ `uart_config` / `uart_write` / `uart_read` / `can_config` / `can_stop` / `can_send` / `can_receive`（CAN 与 UART 一样占用外扩口两脚，需外接收发器） |
| `file` | `read` / `write` / `append` / `size` / `remove` / `exists` / `list` |
| `audio` | `play` / `stop` / `tone`（`tone(freq_hz[, ms])`，默认 200 ms 的提示音）/ `record_start` / `record_stop` / `stream_start` / `write` / `stream_stop`（外部 PCM 流）/ `get_volume` / `set_volume` / `set_mute` |
| `net` | `http_get(url[, opts])` / `http_post(url, body[, opts])`；`opts` 是表 `{timeout = ms, headers = "K: V\r\n...", content_type = "..."}`（也可以直接给一个数字当超时）。成功返回响应体，失败返回 `nil, "HTTP 404"` / 错误名 |
| `mqtt` | `connect` / `publish` / `subscribe` / `disconnect` / `is_connected` |
| `ws` | `connect(url, fn)`（fn 收消息）/ `send` / `is_connected` / `close`；仅文本帧，同一时刻只维持一个连接（重复 connect 先断旧的） |
| `bt` | 从机：`state_name` / `is_connected` / `adv` / `scan_start` / `scan_stop` / `scan_results`（每项 `name` / `bda` / `addr_type` / `rssi`）/ `notify`；HID：`hid_key` / `hid_mouse` / `hid_consumer`（报告要等主机配对完成才发得出去）；中心角色：`gatt_connect(bda[,addr_type])` / `gatt_disconnect` / `gatt_state` / `gatt_discover` / `gatt_services` / `gatt_chars` / `gatt_read` / `gatt_write` / `gatt_subscribe` / `gatt_on_notify(fn(handle,data))`，UUID 用 `"0x180F"` 或完整 128 位字符串 |
| `camera` | `open([jpeg])` / `close` / `is_open` / `capture`；只拍照，`capture(path)` 把当前帧存成 24 位 BMP（RGB565 在设备侧转码，与相机 App 共用 `svc_camera_write_bmp()`；`open(true)` 的 JPEG 模式按码流原样落盘，本板基本用不了） |
| `imu` | `read` / `is_moving` / `orientation` |
| `settings` | `get` / `set` / `get_int` / `set_int`（所有脚本共用 NVS 里的 `script` 命名空间，key 原样使用、自己起名避免撞车） |
| `util` | `base64_encode` / `base64_decode` / `hex_encode` / `hex_decode`（拼 HTTP 头、把二进制塞进文本协议） |
| `json` | `decode(str)`（对象 → 表、数组 → 数组表、null → nil）/ `encode(table)`；解析失败返回 nil |
| `web` | `get(path, fn)` / `post(path, fn)` 注册网页路由（最多 8 条，挂在 `/s` 底下，只在脚本运行期间有效）；`fn(req)` 收到 `{method, path, query（表）, query_string, body}`，返回 `(content_type, body)` 或 `(status, content_type, body)`，响应体上限 24 KB；`url()` 给出 `http://<设备IP>/s`（没连 Wi-Fi 时是 nil）、`ready()` 看控制台是否在监听 |

`ui.image` 只按路径显示图片（路径同样走 `svc_storage` 的存储目录），脚本不做摄像头取帧预览。

`ui.canvas(parent, w, h)` 给脚本一块自己画像素的 RGB565 位图（宽必须是偶数，LVGL 才会直接用它、不做内部拷贝），颜色统一写 `0xRRGGBB`，坐标可以是小数（内部四舍五入 —— 脚本画图基本是 `sin` / `cos` 算出来的）：指针时钟画指针、贪吃蛇画格子都靠它。画布数量上限 8 块，缓冲区随页面一起回收。

脚本不直接调用 Peripherals / Drivers；硬件（GPIO / PWM / I2C / UART / ADC）一律经 `svc_io`。

## 12. 脚本示例

固件内置六个示例（都以真实文件放在 `main/framework/assets/scripts/`，构建期 `EMBED_FILES` 进固件，每次启动释放到 `/sdcard/scripts`、同名覆盖），覆盖四种典型形态：

| 脚本 | 形态 | 说明 |
|------|------|------|
| `counter.lua` | 无界面 | 每秒 `print` 一次计数与时间戳，10 秒后 `sys.exit()` 自己退出 —— 运行日志显示在「运行日志」页 |
| `clock.lua` | 有界面（画布） | 指针式时钟：`cv:circle` 画表盘与刻度、`cv:line` 画时/分/秒三根指针，每秒重画 |
| `snake.lua` | 有界面（画布 + 触摸） | 贪吃蛇：`cv:rect` 画格子，`input.on_click` 拿点按方位决定拐弯，撞墙 / 撞自己结束 |
| `io.lua` | 有界面（设置行） | 外扩接口状态页：一路 I2C（扫描总线上的器件地址），一路外扩口（GPIO10/11 电平，并把复用切换到 PWM / UART / CAN） |
| `gomoku.lua` | 有界面 + 网页 | 五子棋：设备屏幕上显示棋盘与地址，网页（`/s`）用 HTML5 canvas 下棋，两边同步；用到 `web` 能力 |
| `脚本接口参考.txt` | 文档 | 逐个模块列出绑定函数与数量上限，随固件一起释放，方便在板子上直接查 |

示例都以文件形式维护（`release_samples()` 只列"卡上的名字 + 资源起止"），资源文件名必须 ASCII（C 符号名由文件名生成）：`api-reference.txt` 释放到卡上叫「脚本接口参考.txt」；行尾保持原样（.lua 用 LF、这份 .txt 用 CRLF）。

画布示例（指针时钟的骨架）：

```lua
-- @name 指针时钟
-- @desc 画布示例：表盘 + 时/分/秒指针，每秒刷新
-- @perm sys,timer

local W, H = 176, 176
local cx, cy = W / 2, H / 2
local R = W / 2             -- 表盘半径：正好画满画布

local page = ui.page()
local cv = ui.canvas(page, W, H)

local function draw()
    local t = sys.localtime()
    cv:fill(0x121820)
    cv:circle(cx, cy, R, 0x8A93A0)
    cv:line(cx, cy, cx + (R * 0.86) * math.sin(math.rad(t.sec * 6)),
            cy - (R * 0.86) * math.cos(math.rad(t.sec * 6)), 0xE5534B)
end

timer.every(1000, draw)
draw()
```

无界面脚本也一样有"自己的一页"：不调 `ui.page()` 时 `fw_script` 会给一个「运行日志」页显示它的 `print` 输出。脚本退出（用户停止 / `sys.exit()` / 异常）时，`fw_script` 统一回收其中的界面对象、画布缓冲与定时器。
