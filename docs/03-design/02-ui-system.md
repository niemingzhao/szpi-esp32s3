# UI 系统详细设计

UI 系统基于 LVGL v8.3.0 + esp_lvgl_port v1.4.0，由 Framework 层统一封装。

## 1. UI 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                 LVGL Display 320×240                      │
├──────────────────────────────────────────────────────────┤
│ [返回][主页]   10:30   [Wi-Fi][音乐][通知][控制]           │  ← fw_statusbar (28 px)
├──────────────────────────────────────────────────────────┤
│                                                          │
│                     App Window                           │  ← 当前 App
│                                                          │
├──────────────────────────────────────────────────────────┤
│  Overlay（挂在 lv_layer_top()，覆盖状态栏以下整区）:        │
│   • 控制中心 fw_control_center（Wi-Fi / 亮度 / 音量 / 试听）│
│   • 通知中心 fw_notification                              │
│   • Toast / 对话框 fw_ui（对话框为模态，覆盖全屏）          │
│   • 电源菜单（长按 BOOT）                                 │
└──────────────────────────────────────────────────────────┘
```

- 状态栏是唯一常驻浮层，同时承担全局导航（返回 / 主页 / 通知中心 / 控制中心）
- 没有底部虚拟按键栏，也不使用滑动手势
- 开机时状态栏随全局浮层一起隐藏，先全屏展示官方 Logo 与提示音

## 2. fw_statusbar（状态栏）

### 2.1 布局

```
┌────────────────────────────────────────────────────┐
│  10:30       [Wi-Fi] [♪]                            │
└────────────────────────────────────────────────────┘
   左：时间                中：图标
```

### 2.2 接口

```c
esp_err_t fw_statusbar_init(void);
esp_err_t fw_statusbar_set_time(const char *time);     // "10:30"
esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected);
esp_err_t fw_statusbar_set_music_playing(bool on);
esp_err_t fw_statusbar_set_bluetooth(bool on);
```

### 2.3 实现要点

- 订阅 `SVC_EVENT_TIME_CHANGED` 更新时间
- 订阅 `SVC_EVENT_WIFI_*` 更新 Wi-Fi 图标
- 订阅 `SVC_EVENT_AUDIO_*` 更新播放图标
- 订阅 `SVC_EVENT_BRIGHTNESS_CHANGED` 更新亮度
- 左侧返回 / 主页按钮分别调用 `fw_app_mgr_back()` / `fw_app_mgr_back_to_home()`；`fw_app_mgr_back()` 会先询问当前 App 的 `on_back()`（用于 App 内返回上一级页面）
- 右侧通知 / 控制中心按钮打开对应浮层（两者互斥，再点一次关闭）
- 状态栏高度由 `FW_STATUSBAR_H` 定义；App 内容区与浮层都以此为顶部偏移
- 状态栏按钮用 `lv_obj_set_ext_click_area()` 向四周扩大触摸区域（视觉尺寸不变），弥补面板触摸与显示位置的微小偏差
- 蓝牙图标待 BLE 服务落地后显示

## 3. fw_control_center（控制中心）

### 3.1 布局

```
┌────────────────────────────────────────────────────┐
│ ┌──────────────┐  ┌──────────────┐ ┌──────────────┐│
│ │   Wi-Fi       │  │  Bluetooth   │ │  Brightness  ││
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
esp_err_t fw_control_center_toggle(void);
bool fw_control_center_is_visible(void);
```

### 3.3 实现要点

- 默认隐藏，由状态栏的“控制中心”按钮打开；占满状态栏以下的显示区，右上角 × 关闭；内容用纵向 flex 排布，超出高度可滚动
- 亮度滑块调用 `svc_power_set_brightness()`（内部持久化并发布 `SVC_EVENT_BRIGHTNESS_CHANGED`）
- 音量滑块调用 `svc_audio_set_volume()`
- Wi-Fi 磁贴：已连接时点击 = 断开（`svc_net_wifi_stop()`）；未连接时点击 = 用已保存凭据免密重连（`svc_net_wifi_auto_connect()`，没保存过则只打开 Wi-Fi 开关）
- “试听” 按钮调用 `svc_audio_play_tone_async(1000, 300)`，用于确认音频通路
- 长按 Wi-Fi 磁贴弹出"忘记已保存的网络？"确认框，确认后 `svc_net_wifi_forget()` 忘记网络并关闭 Wi-Fi（与 Settings 里的"忘记网络"措辞一致）
- 蓝牙磁贴待 BLE 服务落地；手电筒 / 锁屏磁贴待对应服务

## 4. fw_notification（通知中心）

### 4.1 布局

```
┌────────────────────────────────────────────────────┐
│ 通知中心                                       清空 │
├────────────────────────────────────────────────────┤
│ ⓘ  系统                                10:30       │
│    Wi-Fi 已连接到 XXX                              │
├────────────────────────────────────────────────────┤
│ ⓘ  音乐播放器                            09:15     │
│    正在播放：Canon                                  │
├────────────────────────────────────────────────────┤
│ ⚠  设置                                  昨天      │
│    存储空间不足                                    │
└────────────────────────────────────────────────────┘
```

### 4.2 接口

```c
esp_err_t fw_notification_init(void);
esp_err_t fw_notification_show(void);
esp_err_t fw_notification_hide(void);
esp_err_t fw_notification_toggle(void);
bool fw_notification_is_visible(void);
esp_err_t fw_notification_clear_all(void);
esp_err_t fw_notification_clear(uint32_t id);   // 单条撤销内部走 svc_notification_dismiss()
```

### 4.3 实现要点

- 由状态栏的“通知”按钮打开；占满状态栏以下的显示区，右上角 × 关闭；列表区可滚动
- 订阅 `SVC_EVENT_NOTIFICATION_POSTED`：弹一条 Toast，并在可见时刷新列表
- 订阅 `SVC_EVENT_NOTIFICATION_DISMISSED`：可见时刷新列表
- 列表数据来自 `svc_notification_get()`（按顺序读取；标题 / 正文指向服务内部缓冲区）
- 点击条目调用原 notification 的 on_click 回调，随后关闭通知中心
- 顶部状态栏的通知图标根据是否有通知显示

## 5. 主题系统

### 5.1 调色板

深色 / 浅色两套色板，取色统一走 `fw_theme_color_*()`，不要在界面代码里写死颜色。

| 令牌 | 深色 | 浅色 | 用途 |
|------|------|------|------|
| `bg_primary` | `0x121212` | `0xE7ECF2` | 页面底色 |
| `bg_secondary` | `0x1E1E1E` | `0xFFFFFF` | 状态栏 / 浮层 |
| `bg_card` | `0x2A2A2A` | `0xFFFFFF` | 卡片 / 按钮 |
| `text_primary` | `0xFFFFFF` | `0x161A1F` | 正文 |
| `text_secondary` | `0xBBBBBB` | `0x4C5561` | 次要文字 |
| `text_disabled` | `0x666666` | `0x9AA0A6` | 禁用态 |
| `accent` | `0x4F9EFF` | `0x1D6FD0` | 主色（图标 / 滑块指示条 / 主按钮） |
| `accent2` | `0x9C27B0` | `0x7B1FA2` | 次色 |
| `success` | `0x4CAF50` | `0x2E7D32` | 成功 |
| `warning` | `0xFFC107` | `0xE07B00` | 警告 |
| `error` | `0xF44336` | `0xC62828` | 错误 |
| `divider` | `0x2A2A2A` | `0xD5DCE4` | 分隔线 |
| `border` | `0x333333` | `0xBFCAD6` | 卡片 / 状态栏描边 |

浅色主题下"白卡片 + 浅灰页面"的层次全靠描边，所以卡片、按钮、浮层都要显式设
`border_width = 1` + `border_color = fw_theme_color_border()`；状态栏也用同色画一条底部 1 px 线。

界面里的控件必须显式设色，不要依赖 LVGL 自带主题：`lv_theme_default` 会给 `lv_btn`
加"主色底 + 白字"，我们把按钮底色改成 `bg_card` 之后，标签若不设 `text_color`
就会变成白字白底——深色主题下看不出来，浅色主题下直接消失。
改完界面跑一次自检：

```powershell
python tools/check_ui_colors.py
```

### 5.2 字体

```c
LV_FONT_DECLARE(font_alipuhui20);   // 中文 20 px

// LVGL 内置
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

`fw_theme_apply()` 会同步切换 LVGL 自带主题的明暗，并发布 `SVC_EVENT_THEME_CHANGED`；框架收到后重建状态栏、两个浮层以及所有 App 的界面，因此**立即全局生效**（App 内部页面会回到初始页）。

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
esp_err_t fw_ui_progress_set(lv_obj_t *bar, uint8_t percent);

lv_obj_t *fw_ui_dialog(lv_obj_t *parent, const char *title, const char *msg,
                       fw_dialog_btn_t buttons, fw_dialog_cb_t cb, void *user);
esp_err_t fw_ui_dialog_close(lv_obj_t *dlg);

lv_obj_t *fw_ui_toast(const char *msg, uint32_t duration_ms);

lv_obj_t *fw_ui_list(lv_obj_t *parent, const char *title);
lv_obj_t *fw_ui_list_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *user);
lv_obj_t *fw_ui_grid(lv_obj_t *parent, uint8_t cols, lv_coord_t item_w, lv_coord_t item_h);
```

## 7. 字体与图标资源

### 7.1 字体来源

- 英文 / 数字：LVGL 内置 Montserrat 14（正文）、20（中号）、24（大号）
- 中文：Noto Sans SC（OFL 授权）栅格化的 **14 px（正文）** 与 **16 px（标题）** 子集，位于 `framework/assets/font_cn14.c`、`font_cn16.c`
- 子集只包含界面用字，由 `tools/gen_cn_font.py` 生成；子集里没有的 FontAwesome 符号回退到 Montserrat 14
- 所有字体统一经 `fw_asset_font_cn()` / `fw_asset_font_cn_large()` / `fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()` 获取

### 7.2 图标来源

- 优先 LVGL 内置 symbol（`LV_SYMBOL_PLAY / PAUSE` 等）
- 自定义图标：使用 LVGL image converter 从 PNG 生成 C 数组

### 7.3 图标清单（17 个 App）

每个 App 一个 64×64 图标，作为 C 数组编译进固件。

## 8. 桌面

桌面是"Home"这个特殊 App：

```c
// apps/app_home/app_home.c
static void *home_on_create(void);
static void home_on_destroy(void *ctx);

const fw_app_desc_t app_home_desc = {
    .name = "Home",
    .icon_64 = NULL,          // Scope A 用内置符号；后续替换为 icon_home_64
    .symbol = LV_SYMBOL_HOME,
    .on_create = home_on_create,
    .on_destroy = home_on_destroy,
};
```

桌面内容区：
- 一屏 4×2 = 8 个应用图标（`lv_tileview`，超出可左右滑动翻页），图标顺序即 App 注册顺序（Clock 在左上角第一个）
- 天气小组件（可选）

状态栏（fw_statusbar）是挂在 `lv_layer_top()` 上的全局浮层，由 `fw_init()` 创建，不随屏幕切换消失；桌面内容区位于状态栏以下。

桌面根屏同时是 fw_app_mgr 返回栈的栈底，其 `on_create` 返回根屏对象；`.symbol = LV_SYMBOL_HOME` 作为 Scope A 的内置符号图标（`icon_64` 为 NULL）。

## 9. 启动动画

```
开机:
  1. 全屏黑屏
  2. SZPI-OS Logo 缩放淡入 (300 ms)
  3. Logo 旋转一圈 (500 ms)
  4. 缩放淡出 (300 ms)
  5. 进入桌面
```

实现：`fw_boot_animation()` 单独函数，启动时调用，不依赖 framework。

## 10. 通用交互模式

| 操作 | 行为 |
|------|------|
| 单击 | 触发对应按钮 / 图标 |
| 双击 | 桌面 / 关闭对话框 |
| 长按 | 弹出菜单 / 删除 |
| 滑动 | 不使用（导航由状态栏按钮完成） |
| 状态栏按钮 | 返回、主页、通知中心、控制中心 |

## 11. LVGL 集成要点

### 11.1 Port 配置

LVGL port 与 display 由 `periph_lcd_init()` 负责；touch 的 input device 由 `periph_touch_init()` 注册：

```c
/* periph_lcd_init() 内 */
const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
lvgl_port_init(&lvgl_cfg);

const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = io_handle,        // 来自 drv_st7789_get_io_handle()
    .panel_handle = panel_handle,  // 来自 drv_st7789_get_panel_handle()
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
```

```c
/* periph_touch_init() 内：注册只读缓存的 POINTER input device */
lv_indev_drv_t indev_drv;
lv_indev_drv_init(&indev_drv);
indev_drv.type = LV_INDEV_TYPE_POINTER;
indev_drv.read_cb = touch_indev_read_cb;   // 读取 periph_touch 缓存的坐标
indev_drv.disp = periph_lcd_get_disp();
lv_indev_t *indev = lv_indev_drv_register(&indev_drv);
```

不使用 `lvgl_port_add_touch()`：触摸设备的轮询由 `periph_touch` 的扫描任务独占，以避免 LVGL 与手势任务并发读取同一 `esp_lcd_touch` 句柄造成丢点。

### 11.2 线程安全

- 所有 LVGL API 调用必须经 `lvgl_port_lock(0) / unlock()`
- LVGL tick 由 esp_lvgl_port 自动处理
- 长操作（音频解码、文件读取）放在其他任务，通过队列投递给 LVGL 任务

## 12. 输入路由

### 12.1 手势识别

| 操作 | 默认路由 |
|------|----------|
| 触摸屏幕中央 | 当前 App（经 LVGL input device） |
| 状态栏按钮 | 返回、主页、通知中心、控制中心 |

不使用滑动手势：导航与浮层开关全部由状态栏上的按钮完成（`fw_statusbar`），避免手势与应用内滑动冲突。`periph_touch` 仍会识别 swipe 并发布 `SVC_EVENT_GESTURE_*`，当前没有订阅者。

### 12.2 按键路由

| 按键 | 默认路由 |
|------|----------|
| BOOT 键单击 | 全局 → fw_app_mgr_back() |
| BOOT 键双击 | 全局 → fw_app_mgr_back_to_home() |
| BOOT 键长按 | 全局 → 电源菜单 |

## 13. 过渡动画

应用切换时使用 200-300 ms 淡入淡出动画：

```c
lv_scr_load_anim(new_screen, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
```

动画期间禁用触摸输入（避免误操作）。