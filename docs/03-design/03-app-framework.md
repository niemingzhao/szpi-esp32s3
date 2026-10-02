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

桌面图标不放在 App 目录里：22 个图标集中在 `main/framework/assets/icons_home.c`
（由 `tools/gen_home_icons.py` 生成），App 用描述符的 `icon_64` 引用。

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
    if (s_timer) lv_timer_del(s_timer);
    if (s_root) lv_obj_del(s_root);
    s_root = NULL;
    lvgl_port_unlock();
}

const fw_app_desc_t app_clock_desc = {
    .name = "Clock",
    .title = "时钟",
    .icon_64 = &icon_home_clock,
    .symbol = LV_SYMBOL_BELL,
    .on_create = on_create,
    .on_start = on_start,
    .on_pause = on_pause,
    .on_destroy = on_destroy,
};
```

描述符字段：`name` 是内部标识（英文，启动 / 注册用），`title` 是界面显示名（中文，
桌面网格用），`icon_64` 是桌面彩色图标，`symbol` 只在没有 `icon_64` 时兜底。

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

## 6. App 注册机制

`main/apps/src/app_register.c` 集中注册：

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

App 在 `on_start` 里用 `fw_app_mgr_get_args()` 读取参数。

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
8. **图标与显示名走描述符**：`icon_64` 引用 `main/framework/assets/icons_home.c` 里的彩色图标，`title` 用原始需求「内置应用」里的中文名称
9. **App 内部子页用显示 / 隐藏切换 + `on_back`**：一个 App 只占一张 LVGL 屏，子页做成根对象里的容器；"返回"统一由状态栏返回键触发

## 10. 脚本模型

### 10.1 存放与元信息

- 脚本放在 TF 卡脚本目录，由脚本管理（`app_scripts`）扫描
- 每个脚本可带一段元信息（名称 / 说明 / 入口），没有则用文件名
- 内置示例脚本随固件打包，首次启动释放到脚本目录（同名不覆盖）
- 支持通过链接下载脚本到脚本目录

### 10.2 生命周期

- 同一时刻只运行一个前台脚本：加载 → 执行 → 停止 / 异常退出
- 停止或异常时释放脚本创建的资源（界面对象、定时器、订阅）
- 脚本异常不影响系统，错误信息（行号 + 描述）写日志并提示

### 10.3 运行时

- Lua 5.5 运行时在 `fw_script` 内
- 脚本任务 `script_task`（优先级 4，核心 0）
- 脚本配置经 `svc_settings` 持久化（命名空间 `script`）

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
| `ui` | `page` / `label` / `row` / `image` / `progress` / `toast`；控件方法 `set_text` / `set_value` / `set_src` / `set` |
| `sys` | `uptime` / `now` / `localtime` / `mem`（`mem` 返回内部 SRAM 剩余 / 历史最低与 PSRAM 剩余，单位字节） |
| `timer` | `every` |
| `input` | `on_click`（订阅触摸点击）；BOOT 键事件请用 `event.subscribe("key", fn)` |
| `event` | `subscribe` / `publish`；固定事件名见实现里的 EVENT_MAP（触摸 / BOOT 按键（`key`）/ IMU 姿态与运动 / Wi-Fi / 蓝牙状态 / 亮度 / TF 卡挂载 / 录音结束 / 脚本启停），另有 `user.` 开头的自定义事件名，同一名字稳定映射到同一事件号。`key` 的回调参数是 1 字节字符串，`string.byte(v)` 得事件序号：0 单击 / 1 双击 / 2 长按 |
| `io` | `gpio_write` / `gpio_read` / `pwm_set` / `pwm_stop` / `adc_read` / `i2c_write` / `i2c_read` / `uart_config` / `uart_write` / `uart_read` |
| `file` | `read` / `write` / `remove` / `exists` / `list` |
| `audio` | `play` / `stop` / `tone`（`tone(freq_hz[, ms])`，默认 200 ms 的提示音）/ `record_start` / `record_stop` / `get_volume` / `set_volume` / `set_mute` |
| `net` | `http_get` |
| `mqtt` | `connect` / `publish` / `subscribe` / `disconnect` / `is_connected` |
| `ws` | `connect` / `send` / `is_connected` / `close`；仅文本帧，`send` 发文本 |
| `bt` | `state_name` / `is_connected` / `adv` / `scan_start` / `scan_stop` / `scan_results` / `notify` / `hid_key` / `hid_mouse` / `hid_consumer`；只做从机（广播 / 被连接 / GATT 从机读写与通知），不做中心设备连接与 GATT 客户端读写；HID 报告要等主机配对完成才发得出去 |
| `camera` | `open` / `close` / `is_open` / `capture`；只拍照，`capture(path)` 把照片存到 TF 卡 |
| `imu` | `read` / `is_moving` / `orientation` |
| `settings` | `get` / `set` / `get_int` / `set_int` |

`ui.image` 只按路径显示图片（路径同样走 `svc_storage` 的存储目录），脚本不做摄像头取帧预览。

脚本不直接调用 Peripherals / Drivers；硬件（GPIO / PWM / I2C / UART / ADC）一律经 `svc_io`。

## 12. 脚本示例

```lua
-- @name 时钟
-- @desc 每秒刷新时间，点击屏幕切换 12 / 24 小时制

local hour24 = true
local page = ui.page()
local label = ui.label(page, "00:00:00")

local function on_tick()
    local t = sys.localtime()
    local h = t.hour
    if not hour24 then
        h = h % 12
        if h == 0 then h = 12 end
    end
    label:set_text(string.format("%02d:%02d:%02d", h, t.min, t.sec))
end

input.on_click(function()
    hour24 = not hour24
    ui.toast(hour24 and "24 小时制" or "12 小时制")
end)

timer.every(1000, on_tick)
on_tick()
```

脚本退出（用户停止或异常）时，`fw_script` 统一回收其中的界面对象与定时器。
