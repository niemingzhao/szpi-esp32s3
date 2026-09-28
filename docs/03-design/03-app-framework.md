# 应用框架详细设计

应用框架提供 App 注册、生命周期、消息路由、权限管理等机制。本文档定义 App 编程模型、与 framework / services 的交互约定。

## 1. 应用模板

### 1.1 目录结构

每个 App 一个独立子目录：

```
apps/app_clock/
├── CMakeLists.txt        # 包含 App 源文件到主固件
├── app_clock.h           # App 头文件（可选）
├── app_clock.c           # App 主体
└── assets/               # App 私有资源（可选）
    └── icon_64.c         # 桌面图标
```

### 1.2 最小 App 示例

```c
// apps/app_clock/app_clock.c
#include "fw_app_mgr.h"
#include "lvgl.h"

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_timer_t *s_refresh_timer = NULL;

static void refresh_cb(lv_timer_t *t) {
    // 更新时间显示
}

static void *on_create(void) {
    lvgl_port_lock(0);
    s_root = lv_obj_create(NULL);
    s_time_label = lv_label_create(s_root);
    lv_obj_center(s_time_label);
    s_refresh_timer = lv_timer_create(refresh_cb, 500, NULL);
    lvgl_port_unlock();
    return s_root;
}

static void on_start(void *ctx) {
    // 进入前台
}

static void on_pause(void *ctx) {
    // 进入后台
}

static void on_resume(void *ctx) {
    // 恢复
}

static void on_destroy(void *ctx) {
    lvgl_port_lock(0);
    lv_timer_del(s_refresh_timer);
    lv_obj_del(s_root);
    lvgl_port_unlock();
}

FW_APP_REGISTER(
    .name = "Clock",
    .icon_64 = &icon_clock_64,
    .symbol = LV_SYMBOL_BELL,   // icon_64 为 NULL 时使用内置符号
    .on_create = on_create,
    .on_start = on_start,
    .on_pause = on_pause,
    .on_resume = on_resume,
    .on_destroy = on_destroy,
);
```

## 2. 应用生命周期详解

### 2.1 状态机

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

### 2.2 各阶段职责

| 阶段 | App 应该做 | App 不应该做 |
|------|-----------|--------------|
| `on_create` | 创建所有 LVGL 对象，分配资源 | 调用阻塞 API，订阅事件 |
| `on_start` | 订阅事件，启动计时器 | 创建对象（应在 create 完成） |
| `on_pause` | 取消订阅，暂停计时器，保存关键状态 | 删除 LVGL 对象 |
| `on_resume` | 重新订阅，重启计时器，恢复状态 | 重新创建 LVGL 对象 |
| `on_destroy` | 释放所有资源（LVGL 对象、互斥锁、任务、订阅） | 调用阻塞 API |

### 2.3 重要约束

- **`on_create` 必须快速**（< 100 ms），避免拖慢桌面启动
- **LVGL 操作必须在 `lvgl_port_lock(0) / unlock()` 之间**
- **资源分配失败必须优雅降级**：如果 PSRAM 不够，应该显示错误页而非崩溃
- **所有定时器必须在 `on_destroy` 删除**，否则下一次启动会泄漏

## 3. App 与 Services 交互

### 3.1 播放音乐

```c
static void btn_play_cb(lv_event_t *e) {
    const char *path = "/sdcard/music/test.mp3";
    svc_audio_source_t src = {
        .type = SVC_AUDIO_SRC_FILE,
        .uri = path,
    };
    esp_err_t ret = svc_audio_play(&src);
    if (ret != ESP_OK) {
        svc_notification_t noti = {
            .type = SVC_NOTI_TYPE_ERROR,
            .title = "播放失败",
            .message = esp_err_to_name(ret),
        };
        svc_notification_post(&noti);
    }
}
```

### 3.2 订阅事件

```c
static void on_evt(const svc_event_t *evt, void *user) {
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

### 3.3 持久化设置

```c
// 读取
uint32_t volume;
svc_settings_get_u32("app_music", "volume", &volume, 80);  // 默认 80

// 写入
svc_settings_set_u32("app_music", "volume", new_volume);
```

### 3.4 提示音

```c
// 单次提示
svc_audio_play_tone_async(1000, 100);   // 1 kHz, 100 ms
```

## 4. App 内部状态管理

### 4.1 App Context 结构体

```c
typedef struct {
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_timer_t *timer;
    bool is_24h;
    char current_tz[32];
} clock_ctx_t;

static clock_ctx_t s_ctx = {0};

static void *clock_on_create(void) {
    lvgl_port_lock(0);
    s_ctx.root = lv_obj_create(NULL);
    // 创建其他对象
    s_ctx.timer = lv_timer_create(clock_refresh, 500, NULL);
    lvgl_port_unlock();
    return s_ctx.root;   // on_create 返回根屏对象，作为各生命周期回调的 ctx
}
```

### 4.2 多实例（不常用）

通过 `fw_app_mgr_launch(name, args)` 第二个参数支持多实例（Phase 2）。

## 5. App 注册机制

### 5.1 手动注册（Phase 1）

```c
// apps/src/app_register.c
#include "fw_app_mgr.h"

extern const fw_app_desc_t app_home_desc;
extern const fw_app_desc_t app_clock_desc;
extern const fw_app_desc_t app_music_desc;
// ...

void app_register_all(void) {
    fw_app_mgr_register(&app_home_desc);
    fw_app_mgr_register(&app_clock_desc);
    fw_app_mgr_register(&app_music_desc);
    // ...
}
```

### 5.2 自动注册（Phase 2，可选）

通过 Section 宏实现：

```c
// fw_app_mgr.h
#define FW_APP_REGISTER(desc) \
    static const fw_app_desc_t __app_desc_##__LINE__ \
        __attribute__((used, section(".app_desc_section"))) = desc

// main.c
extern const fw_app_desc_t *__app_desc_section_start;
extern const fw_app_desc_t *__app_desc_section_end;

void app_register_all(void) {
    for (const fw_app_desc_t *d = &__app_desc_section_start; 
         d < &__app_desc_section_end; d++) {
        fw_app_mgr_register(d);
    }
}
```

## 6. App 之间通信

### 6.1 直接调用

App A 调用 App B 的特定函数（需 B 暴露 API）。

### 6.2 事件总线

```c
// App A 发送
svc_event_t evt = {
    .id = SVC_EVENT_USER_BASE,
    .data = my_data,
    .data_len = sizeof(my_data_t),
};
svc_event_bus_publish(SVC_EVENT_USER_BASE, &my_data, sizeof(my_data_t));

// App B 接收
svc_event_bus_subscribe(SVC_EVENT_USER_BASE, on_user_evt, NULL);
```

### 6.3 URI 启动

```c
// 通过 URI 启动其他 App
fw_app_mgr_launch("Music");
fw_app_mgr_launch("File?path=/sdcard/music");
```

## 7. App 调试

### 7.1 串口日志

```c
static const char *TAG = "app.clock";
ESP_LOGI(TAG, "Clock created");
ESP_LOGE(TAG, "Failed to parse: %s", input);
```

### 7.2 性能监控

通过 `dbg_perf_monitor_start()` 显示 FPS / 内存叠层。

## 8. App 示例：时钟（完整）

```c
// apps/app_clock/app_clock.c

#include "fw_app_mgr.h"
#include "fw_theme.h"
#include "svc_time.h"
#include "svc_event_bus.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_alipuhui20);
LV_FONT_DECLARE(font_alipuhui48);

typedef struct {
    lv_obj_t *root;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_timer_t *timer;
} clock_ctx_t;

static clock_ctx_t s_ctx = {0};

static void refresh_cb(lv_timer_t *t) {
    int64_t now = svc_time_now();
    struct tm t_info;
    localtime_r(&now, &t_info);

    char time_str[8], date_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", &t_info);
    strftime(date_str, sizeof(date_str), "%m月%d日 星期%w", &t_info);

    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_ctx.time_label, "%s", time_str);
    lv_label_set_text_fmt(s_ctx.date_label, "%s", date_str);
    lvgl_port_unlock();
}

static void on_time_synced(const svc_event_t *evt, void *user) {
    refresh_cb(NULL);
}

static void *clock_on_create(void) {
    lvgl_port_lock(0);

    s_ctx.root = lv_obj_create(NULL);
    lv_obj_set_size(s_ctx.root, 320, 240 - 24);
    lv_obj_set_style_bg_color(s_ctx.root, fw_theme_color_bg_primary(), 0);

    s_ctx.time_label = lv_label_create(s_ctx.root);
    lv_obj_set_style_text_font(s_ctx.time_label, &font_alipuhui48, 0);
    lv_obj_set_style_text_color(s_ctx.time_label, fw_theme_color_text_primary(), 0);
    lv_obj_align(s_ctx.time_label, LV_ALIGN_CENTER, 0, -20);

    s_ctx.date_label = lv_label_create(s_ctx.root);
    lv_obj_set_style_text_font(s_ctx.date_label, &font_alipuhui20, 0);
    lv_obj_set_style_text_color(s_ctx.date_label, fw_theme_color_text_secondary(), 0);
    lv_obj_align(s_ctx.date_label, LV_ALIGN_CENTER, 0, 40);

    s_ctx.timer = lv_timer_create(refresh_cb, 1000, NULL);

    lvgl_port_unlock();

    refresh_cb(NULL);
    return &s_ctx;
}

static void clock_on_start(void *ctx) {
    svc_event_bus_subscribe(SVC_EVENT_TIME_CHANGED, on_time_synced, NULL);
    if (s_ctx.timer) lv_timer_resume(s_ctx.timer);
}

static void clock_on_pause(void *ctx) {
    svc_event_bus_unsubscribe(SVC_EVENT_TIME_CHANGED, on_time_synced);
    if (s_ctx.timer) lv_timer_pause(s_ctx.timer);
}

static void clock_on_resume(void *ctx) {
    svc_event_bus_subscribe(SVC_EVENT_TIME_CHANGED, on_time_synced, NULL);
    if (s_ctx.timer) lv_timer_resume(s_ctx.timer);
}

static void clock_on_destroy(void *ctx) {
    lvgl_port_lock(0);
    if (s_ctx.timer) lv_timer_del(s_ctx.timer);
    if (s_ctx.root) lv_obj_del(s_ctx.root);
    s_ctx = (clock_ctx_t){0};
    lvgl_port_unlock();
}

FW_APP_REGISTER(
    .name = "Clock",
    .icon_64 = &icon_clock_64,
    .on_create = clock_on_create,
    .on_start = clock_on_start,
    .on_pause = clock_on_pause,
    .on_resume = clock_on_resume,
    .on_destroy = clock_on_destroy,
);
```

## 9. App 开发最佳实践

1. **永远在 LVGL 操作前后加锁**
2. **永远在 on_destroy 释放所有资源**（LVGL 对象、定时器、订阅、互斥锁）
3. **避免在 on_create 中做耗时操作**
4. **使用 app ctx 结构体而非全局变量**
5. **配置项用 svc_settings，命名空间用 app_<name>**
6. **错误必须优雅处理**（不崩溃，显示提示）
7. **资源路径用绝对路径**：`/sdcard/...` 或 `/internal/...`
8. **图标 64×64 PNG，资源压缩到 PSRAM**

## 10. 启动参数

App 启动时可携带参数：

```c
// 启动方
fw_app_mgr_launch("File?path=/sdcard/music&mode=list");

// 接收方（在 on_start 中解析）
static void on_start(void *ctx) {
    // 从 URI 解析参数
}
```

## 11. 权限管理（Phase 3）

每个 App 声明需要的权限：

```c
typedef enum {
    FW_PERM_STORAGE_READ,
    FW_PERM_STORAGE_WRITE,
    FW_PERM_NETWORK,
    FW_PERM_BLUETOOTH,
    FW_PERM_CAMERA,
    FW_PERM_MIC,
} fw_perm_t;

const fw_perm_t app_perms[] = {
    FW_PERM_STORAGE_READ,
    FW_PERM_NETWORK,
};
```

## 12. 应用沙箱（Phase 3）

ESP32-S3 无 MMU，软件沙箱措施：

- 每个 App 独立 NVS 命名空间
- 每个 App 独立文件目录（`/internal/apps/<name>/`、`/sdcard/<name>/`）
- App 崩溃由 Task Watchdog 检测并重启系统
- 不允许 App 直接调用 Peripherals / Drivers（编译时约束）