# 应用与脚本框架详细设计

系统提供两套应用模型：**原生 App**（C 编写，编译进固件，由 `fw_app_mgr` 管理）与**脚本**（Lua 编写，放 TF 卡，由 `fw_script` 管理）。本文档定义两者的编程模型与约定。

## 1. 应用模型对比

| 维度 | 原生 App | 脚本 |
|------|----------|------|
| 语言 | C | Lua 5.4 |
| 存放 | 编译进固件（`main/apps/`） | TF 卡脚本目录 |
| 注册 | `fw_app_mgr_register()` | 脚本管理器扫描目录 |
| 生命周期 | create / start / pause / resume / destroy / back | 加载 / 执行 / 停止 |
| 入口 | `fw_app_mgr_launch("Name")` | `fw_script_run(path)` |
| 能力 | Framework + Services | 通过绑定访问 Framework / Services / `svc_io` |

两者共用事件总线、配置系统与存储；桌面只展示原生 App，脚本统一在脚本管理器里运行。

## 2. 原生 App 模板

### 2.1 目录结构

每个 App 一个独立子目录：

```
main/apps/app_clock/
├── app_clock.h           # App 头文件（可选）
├── app_clock.c           # App 主体
└── assets/               # App 私有资源（可选）
    └── icon_64.c         # 桌面图标
```

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
    .symbol = LV_SYMBOL_BELL,
    .on_create = on_create,
    .on_start = on_start,
    .on_pause = on_pause,
    .on_destroy = on_destroy,
};
```

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

注册顺序即桌面图标顺序（Home 之外）。

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
5. **配置项用 svc_settings，命名空间用 `app_<name>`**
6. **错误必须优雅处理**（不崩溃，用 `fw_ui_toast()` 提示）
7. **资源路径用绝对路径**：`/sdcard/...` 或 `/internal/...`
8. **图标 64×64，编译进固件**
9. **App 内部子页用显示 / 隐藏切换 + `on_back`**：一个 App 只占一张 LVGL 屏，子页做成根对象里的容器；"返回"统一由状态栏返回键触发

## 10. 脚本模型

### 10.1 存放与元信息

- 脚本放在 TF 卡脚本目录，由脚本管理器（`app_scripts`）扫描
- 每个脚本可带一段元信息（名称 / 说明 / 入口），没有则用文件名
- 内置示例脚本随固件打包，首次启动释放到脚本目录（同名不覆盖）
- 支持通过链接下载脚本到脚本目录

### 10.2 生命周期

- 同一时刻只运行一个前台脚本：加载 → 执行 → 停止 / 异常退出
- 停止或异常时释放脚本创建的资源（界面对象、定时器、订阅）
- 脚本异常不影响系统，错误信息（行号 + 描述）写日志并提示

### 10.3 运行时

- Lua 5.4 运行时在 `fw_script` 内
- 脚本任务 `script_task`（优先级 4，核心 0）
- 脚本配置经 `svc_settings`（命名空间 `script` 或脚本私有）持久化

## 11. 脚本能力绑定

| 能力 | 绑定到 |
|------|--------|
| 显示 / 界面 | `fw_ui`（页面 / 控件 / 图片 / 进度 / Toast / 对话框） |
| 输入 | `fw_input`、`periph_touch` 缓存的触摸、IMU 姿态、按键 |
| 音频播放 / 录音 | `svc_audio` |
| 摄像头预览 / 拍照 | `svc_camera` |
| GPIO / PWM / I2C / UART / ADC | `svc_io` |
| 文件读写 | `svc_storage` |
| 网络 | `svc_net` / `svc_mqtt` / `svc_ws` |
| 蓝牙 BLE | `svc_bt` |
| 时间 / 系统信息 | `svc_time` / `svc_sysinfo` |
| 事件订阅 / 发布 | `svc_event_bus` |
| 系统通知（Toast） | `fw_ui` |

脚本不直接调用 Peripherals / Drivers；硬件能力一律经 `svc_io`。

## 12. 脚本示例

```lua
-- 每秒刷新一次时间，点击屏幕切换 12 / 24 小时制

local hour24 = true

local page = ui.page()
local label = ui.label(page, "00:00")

function on_tick()
    local t = sys.localtime()
    label:set_text(string.format("%02d:%02d:%02d", t.hour, t.min, t.sec))
end

input.on_click(function()
    hour24 = not hour24
    ui.toast(hour24 and "24 小时制" or "12 小时制")
end)

timer.every(1000, on_tick)
on_tick()
```

脚本退出（用户停止或异常）时，`fw_script` 统一回收其中的界面对象与定时器。
