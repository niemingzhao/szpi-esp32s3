# UI 系统详细设计

UI 系统基于 LVGL v8.3.0 + esp_lvgl_port v1.4.0，由 Framework 层统一封装。

## 1. UI 系统架构

```
┌──────────────────────────────────────────────────────────┐
│              LVGL Display 320x240                         │
├──────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────┐ │
│  │           Status Bar (24px)         │ │  ← fw_statusbar                       │
│  ├─────────────────────────────────────────────────┤ │
│  │                                                 │ │
│  │       App Window                                 │ │  ← 当前 app
│  │                                                 │ │
│  ├─────────────────────────────────────────────────┤ │
│  │     虚拟按键栏 (BACK / HOME, 24-32px)        │ │  ← fw_input
│  └─────────────────────────────────────────────────┘ │
│                                                          │
│  Overlay Layers:                                         │
│   • Control Center (下拉覆盖)                          │  ← fw_control_center
│   • Notification Center (下拉覆盖)                    │  ← fw_notification
│   • Power Menu (长按 BOOT)                            │
│   • Toast (短提示)                                     │
└──────────────────────────────────────────────────────────┘
```

## 2. fw_statusbar（状态栏）

### 2.1 布局

```
┌────────────────────────────────────────────────────┐
│  10:30       [WiFi] [♪]                        ●● │
└────────────────────────────────────────────────────┘
   左：时间                中：图标           右：状态点
```

### 2.2 接口

```c
esp_err_t fw_statusbar_init(void);
esp_err_t fw_statusbar_set_time(const char *time);     // "10:30"
esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected);
esp_err_t fw_statusbar_set_battery(uint8_t percent);
esp_err_t fw_statusbar_set_music_playing(bool on);
esp_err_t fw_statusbar_set_bluetooth(bool on);
```

### 2.3 实现要点

- 订阅 `SVC_EVENT_TIME_CHANGED` 更新时间
- 订阅 `SVC_EVENT_WIFI_*` 更新 WiFi 图标
- 订阅 `SVC_EVENT_AUDIO_*` 更新播放图标
- 订阅 `SVC_EVENT_BRIGHTNESS_CHANGED` 更新亮度

## 3. fw_control_center（控制中心）

### 3.1 布局

```
┌────────────────────────────────────────────────────┐
│ ┌──────────────┐  ┌──────────────┐ ┌──────────────┐│
│ │   WiFi       │  │  Bluetooth   │ │  Brightness  ││
│ │   ●  ON      │  │   ○  OFF     │ │ ━━━━○─────   ││
│ └──────────────┘  └──────────────┘ └──────────────┘│
│ ┌──────────────┐  ┌──────────────┐ ┌──────────────┐│
│ │   Volume     │  │   Flashlight │ │   Lock       ││
│ │ ━━━━━○───    │  │   ○  OFF     │ │   ○  OFF     ││
│ └──────────────┘  └──────────────┘ └──────────────┘│
└────────────────────────────────────────────────────┘
```

### 3.2 接口

```c
esp_err_t fw_control_center_init(void);
esp_err_t fw_control_center_show(void);
esp_err_t fw_control_center_hide(void);
```

### 3.3 实现要点

- 默认隐藏，三指下滑或特定手势打开
- 滑块亮度调用 `svc_settings_set(BRIGHTNESS)`
- 滑块音量调用 `svc_audio_set_volume()`
- WiFi / Bluetooth 开关调用 `svc_net_*`

## 4. fw_notification（通知中心）

### 4.1 布局

```
┌────────────────────────────────────────────────────┐
│ 通知中心                                       清空 │
├────────────────────────────────────────────────────┤
│ ⓘ  系统                                10:30       │
│    WiFi 已连接到 XXX                              │
├────────────────────────────────────────────────────┤
│ ⓘ  音乐播放器                            09:15     │
│    正在播放：Canon                                  │
├────────────────────────────────────────────────────┤
│ ⚠  设置                                  昨天      │
│    电量低于 20%                                    │
└────────────────────────────────────────────────────┘
```

### 4.2 接口

```c
esp_err_t fw_notification_init(void);
esp_err_t fw_notification_show(void);
esp_err_t fw_notification_hide(void);
esp_err_t fw_notification_clear_all(void);
esp_err_t fw_notification_clear(uint32_t id);
```

### 4.3 实现要点

- 订阅 `SVC_EVENT_NOTIFICATION_POSTED` 添加条目
- 订阅 `SVC_EVENT_NOTIFICATION_DISMISSED` 删除条目
- 点击条目调用原 notification 的 on_click 回调
- 顶部状态栏的通知图标根据是否有未读显示

## 5. 主题系统

### 5.1 颜色定义（默认深色主题）

```c
#define COLOR_BG_PRIMARY       lv_color_hex(0x121212)
#define COLOR_BG_SECONDARY     lv_color_hex(0x1E1E1E)
#define COLOR_BG_CARD          lv_color_hex(0x2A2A2A)
#define COLOR_BG_HOVER         lv_color_hex(0x3A3A3A)

#define COLOR_TEXT_PRIMARY     lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_SECONDARY   lv_color_hex(0xBBBBBB)
#define COLOR_TEXT_DISABLED    lv_color_hex(0x666666)

#define COLOR_ACCENT           lv_color_hex(0x4F9EFF)
#define COLOR_ACCENT2          lv_color_hex(0x9C27B0)
#define COLOR_SUCCESS          lv_color_hex(0x4CAF50)
#define COLOR_WARNING          lv_color_hex(0xFFC107)
#define COLOR_ERROR            lv_color_hex(0xF44336)

#define COLOR_DIVIDER          lv_color_hex(0x2A2A2A)
#define COLOR_BORDER           lv_color_hex(0x333333)
```

### 5.2 字体

```c
LV_FONT_DECLARE(font_alipuhui20);   // 中文 20px
LV_FONT_DECLARE(font_alipuhui16);   // 中文 16px

// LVGL 内置
//   lv_font_montserrat_12
//   lv_font_montserrat_14
//   lv_font_montserrat_16
//   lv_font_montserrat_20
//   lv_font_montserrat_24
//   lv_font_montserrat_32
```

### 5.3 接口

```c
esp_err_t fw_theme_init(void);
esp_err_t fw_theme_apply(fw_theme_t theme);  // FW_THEME_DARK / FW_THEME_LIGHT
fw_theme_t fw_theme_current(void);

lv_color_t fw_theme_color_bg_primary(void);
lv_color_t fw_theme_color_accent(void);
```

## 6. 通用 UI 组件

封装在 Framework 层：

| 组件 | 用途 |
|------|------|
| 进度条 | 长操作反馈（OTA / 文件复制 / 加载） |
| 对话框 | 二次确认 / 警告 / 信息提示 |
| Toast | 短提示（3 秒自动消失） |
| 列表 | 设置项 / 文件浏览 |
| 网格 | 应用图标 / 图片缩略图 |

每个组件提供统一 API：

```c
lv_obj_t *fw_ui_progress_bar(lv_obj_t *parent, const char *title);
lv_obj_t *fw_ui_dialog(lv_obj_t *parent, const char *title, const char *msg, fw_dialog_btn_t buttons);
lv_obj_t *fw_ui_toast(const char *msg, uint32_t duration_ms);
```

## 7. 字体与图标资源

### 7.1 字体来源

- 英文：LVGL 内置 Montserrat 12/14/16/20/24/32
- 中文：阿里巴巴普惠体（已用于 14-handheld）
- 中文字体子集化：通过 LVGL font converter 生成

### 7.2 图标来源

- 优先 LVGL 内置 symbol（`LV_SYMBOL_PLAY / PAUSE` 等）
- 自定义图标：使用 LVGL image converter 从 PNG 生成 C 数组

### 7.3 图标清单（16 个 app）

每个 app 一个 64×64 图标，作为 C 数组编译进固件。

## 8. 桌面

桌面是"Home"这个特殊 app：

```c
// apps/app_home/app_home.c
FW_APP_REGISTER(
    .name = "Home",
    .icon_64 = &icon_home_64,
    .on_create = home_create,
    .on_destroy = home_destroy,
);
```

桌面包含：
- 顶部状态栏（fw_statusbar 嵌入）
- 天气小组件（可选）
- 应用网格（4×2 = 8 个图标，从 fw_app_mgr_list 获取）
- 底部虚拟按键栏

## 9. 启动动画

```
开机:
  1. 全屏黑屏
  2. SZPI-OS Logo 缩放淡入 (300ms)
  3. Logo 旋转一圈 (500ms)
  4. 缩放淡出 (300ms)
  5. 进入桌面
```

实现：`fw_boot_animation()` 单独函数，启动时调用，不依赖 framework。

## 10. 通用交互模式

| 操作 | 行为 |
|------|------|
| 单击 | 触发对应按钮 / 图标 |
| 双击 | 桌面 / 关闭对话框 |
| 长按 | 弹出菜单 / 删除 |
| 滑动（左 / 右） | 切换 app / 关闭对话框 |
| 下滑 | 打开控制中心 |
| 上滑 | 关闭控制中心 |

## 11. LVGL 集成要点

### 11.1 Port 配置

```c
const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
lvgl_port_init(&lvgl_cfg);

const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = io_handle,        // 来自 hal_lcd_get_disp
    .panel_handle = panel_handle,
    .buffer_size = 320 * 20,       // 20 行高
    .double_buffer = false,
    .hres = 320,
    .vres = 240,
    .monochrome = false,
    .rotation = {
        .swap_xy = true,
        .mirror_x = true,
        .mirror_y = false,
    },
    .flags = {
        .buff_dma = false,
        .buff_spiram = true,        // 帧缓冲强制 PSRAM
    }
};

lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);

const lvgl_port_touch_cfg_t touch_cfg = {
    .disp = disp,
    .handle = tp,                  // 来自 hal_touch
};

lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
```

### 11.2 线程安全

- 所有 LVGL API 调用必须经 `lvgl_port_lock(0) / unlock()`
- LVGL tick 由 esp_lvgl_port 自动处理
- 长操作（音频解码、文件读取）放在其他任务，通过队列投递给 LVGL 任务

## 12. 输入路由

### 12.1 手势识别

| 手势 | 默认路由 |
|------|----------|
| 右边缘左滑 | 全局 → fw_app_mgr_back() |
| 三指下滑 | 全局 → 打开控制中心 |
| 单指下滑 | 全局 → 打开通知中心 |
| 触摸屏幕中央 | 当前 app（经 LVGL input device） |

### 12.2 按键路由

| 按键 | 默认路由 |
|------|----------|
| BOOT 键单击 | 全局 → fw_app_mgr_back() |
| BOOT 键双击 | 全局 → fw_app_mgr_back_to_home() |
| BOOT 键长按 | 全局 → 电源菜单 |

## 13. 过渡动画

应用切换时使用 200-300ms 淡入淡出动画：

```c
lv_scr_load_anim(new_screen, LV_SCR_LOAD_ANIM_FADE_ON, 250, 0, NULL);
```

动画期间禁用触摸输入（避免误操作）。