# UI 系统详细设计

UI 系统基于 LVGL 9 + esp_lvgl_port 2.x，由 Framework 层统一封装。

新增或修改界面前，先读第 12 节《UI 视觉规范》：尺寸、圆角、间距、配色令牌与交互按那一节执行，保持与现有界面同一套观感。

## 1. UI 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                 LVGL Display 320×240                      │
├──────────────────────────────────────────────────────────┤
│ [返回][主页]  10:30   [Wi-Fi][BLE][声][录][摄][卡]         │  ← fw_statusbar (28 px)
├──────────────────────────────────────────────────────────┤
│                                                          │
│              当前界面（原生 App 或脚本）                   │
│                                                          │
├──────────────────────────────────────────────────────────┤
│  Overlay（挂在 lv_layer_top()）:                           │
│   • Toast / 对话框 fw_ui                                  │
│   • 电源菜单（长按 BOOT）                                 │
└──────────────────────────────────────────────────────────┘
```

- 状态栏是唯一常驻浮层，承担全局导航（返回 / 主页）
- 没有底部虚拟按键栏，也不使用滑动手势
- 开机时状态栏随浮层一起隐藏，先全屏展示 Logo 与提示音

## 2. fw_statusbar（状态栏）

### 2.1 布局

```
┌────────────────────────────────────────────────────┐
│ [返回][主页]        10:30      [Wi-Fi][BLE][声][录][摄][卡] │
└────────────────────────────────────────────────────┘
   左：返回 / 主页        中：时间        右：状态图标
```

### 2.2 接口

```c
esp_err_t fw_statusbar_init(void);
esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected);
esp_err_t fw_statusbar_set_bluetooth(bool on);
esp_err_t fw_statusbar_set_playing(bool on);
esp_err_t fw_statusbar_set_recording(bool on);
esp_err_t fw_statusbar_set_camera(bool on);
esp_err_t fw_statusbar_set_sd(bool mounted);
```

### 2.3 实现要点

- 订阅事件更新图标：`SVC_EVENT_TIME_CHANGED`、`SVC_EVENT_WIFI_*`、`SVC_EVENT_BT_STATE_CHANGED`、`SVC_EVENT_AUDIO_*`、`SVC_EVENT_SD_*`
- 左侧返回 / 主页按钮分别调用 `fw_app_mgr_back()` / `fw_app_mgr_back_to_home()`；`fw_app_mgr_back()` 会先询问当前 App 的 `on_back()`（用于 App 内返回上一级页面）
- 状态栏高度由 `FW_STATUSBAR_H` 定义；App 内容区与浮层都以此为顶部偏移
- 状态栏按钮用 `lv_obj_set_ext_click_area()` 向四周扩大触摸区域（视觉尺寸不变），弥补面板触摸与显示位置的微小偏差
- 蓝牙图标跟随 BLE 状态：`svc_bt_get_state() != SVC_BT_STATE_OFF` 时点亮

## 3. 主题系统

### 3.1 调色板

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

界面里的控件必须显式设色，不要依赖 LVGL 自带主题：`lv_theme_default` 会给 `lv_button`
加"主色底 + 白字"，我们把按钮底色改成 `bg_card` 之后，标签若不设 `text_color`
就会变成白字白底——深色主题下看不出来，浅色主题下直接消失。
改完界面跑一次自检：

```powershell
python tools/check_ui_colors.py
```

### 3.2 接口

```c
esp_err_t fw_theme_init(void);
esp_err_t fw_theme_apply(fw_theme_t theme);  // FW_THEME_DARK / FW_THEME_LIGHT
fw_theme_t fw_theme_current(void);

lv_color_t fw_theme_color_bg_primary(void);
lv_color_t fw_theme_color_accent(void);
```

`fw_theme_apply()` 会同步切换 LVGL 自带主题的明暗，并发布 `SVC_EVENT_THEME_CHANGED`；框架收到后重建状态栏、浮层以及所有 App / 脚本的界面，因此**立即全局生效**（界面回到初始页）。

## 4. 通用 UI 组件

封装在 Framework 层：

| 组件 | 用途 |
|------|------|
| 进度条 | 长操作反馈（下载 / 文件复制 / 加载） |
| 对话框 | 二次确认 / 警告 / 信息提示 |
| Toast | 短提示（自动消失） |
| 列表 | 设置项 / 文件浏览 |
| 网格 | 应用图标 / 图片缩略图 |
| 页面 / 整行入口 / 滑块行 | `fw_ui_page()` / `fw_ui_row_btn()` / `fw_ui_slider_row()`，界面页面的标准骨架（规格见 12.1） |

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

## 5. 字体与图标资源

### 5.1 字体

- 英文 / 数字：LVGL 内置 Montserrat 14（正文）、20（中号）、24（大号）
- 中文：Noto Sans SC 栅格化的 **14 px（正文）** 与 **16 px（标题）** 子集，位于 `main/framework/assets/font_cn14.c`、`font_cn16.c`
- 子集只包含界面用字，由 `tools/gen_cn_font.py` 生成（LVGL 9 字体格式）；子集里没有的符号回退到 Montserrat 14
- 所有字体统一经 `fw_asset_font_cn()` / `fw_asset_font_cn_large()` / `fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()` 获取

### 5.2 图标

- 优先 LVGL 内置 symbol（`LV_SYMBOL_PLAY` / `LV_SYMBOL_PAUSE` 等）
- 自定义图标：用 LVGLImage.py 从 PNG 生成 C 数组

### 5.3 图标清单（22 个 App）

每个 App 一个 64×64 图标，作为 C 数组编译进固件；桌面图标用内置符号兜底。

## 6. 桌面

桌面是"Home"这个特殊 App：

```c
// main/apps/app_home/app_home.c
const fw_app_desc_t app_home_desc = {
    .name = "Home",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_HOME,
    .on_create = home_on_create,
    .on_destroy = home_on_destroy,
};
```

桌面内容区：
- 一屏 4×2 = 8 个应用图标（`lv_tileview`，超出可左右滑动翻页），图标顺序即 App 注册顺序
- 状态栏（fw_statusbar）挂在 `lv_layer_top()` 上，由 `fw_init()` 创建，不随屏幕切换消失
- 桌面根屏同时是 fw_app_mgr 返回栈的栈底

## 7. 脚本界面

脚本通过 `fw_script` 的绑定创建界面，与原生 App 共用同一套 LVGL display：

- 脚本创建的对象挂在脚本自己的根屏 / 容器下；脚本退出时统一删除
- 可用能力：页面 / 文字 / 图片 / 进度条 / 列表 / Toast / 对话框
- 界面操作在 LVGL 任务上下文执行（`fw_script` 内部加锁），脚本不直接碰 LVGL
- 主题切换时脚本界面随之重建

脚本的界面风格仍遵守第 12 节《UI 视觉规范》：取色走 `fw_theme_color_*()`，字体走 `fw_asset_font_*()`。

## 8. 启动动画

```
开机:
  1. 隐藏状态栏等全局浮层，切到全屏黑底
  2. 居中显示立创官方 120×120 Logo（静态展示）
  3. 解除静音后等功放稳定，再播一声 1 kHz / 300 ms 提示音（全程约 1.35 s）
  4. 恢复浮层，由 main 切到桌面
```

实现：`fw_boot_animation()`，用 `fw_asset` 的 Logo 资源与 `svc_audio` 的提示音接口。

## 9. 通用交互模式

| 操作 | 行为 |
|------|------|
| 单击 | 触发对应按钮 / 图标 |
| 长按 | 弹出菜单 / 删除 |
| 滑动 | 不使用（列表 / 桌面翻页除外） |
| 状态栏按钮 | 返回、主页 |

## 10. LVGL 集成要点

### 10.1 Port 配置

LVGL port 与 display 由 `periph_lcd_init()` 负责；touch 的 input device 由 `periph_touch_init()` 注册：

```c
/* periph_lcd_init() 内：初始化 LVGL 并注册显示（esp_lvgl_port 2.x / LVGL 9） */
const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
lvgl_port_init(&lvgl_cfg);

const lvgl_port_display_cfg_t disp_cfg = {
    .io_handle = io_handle,        // 来自 drv_st7789_get_io_handle()
    .panel_handle = panel_handle,  // 来自 drv_st7789_get_panel_handle()
    .buffer_size = 320 * 10,       // 10 行高
    .double_buffer = false,
    .hres = 320,
    .vres = 240,
    .monochrome = false,
    .flags = {
        .buff_dma = true,
        .buff_spiram = false,      // 绘制缓冲放内置 DMA 内存（见 AGENTS.md 4.2）
    }
};
lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);

/* 换成带错误处理的 flush 回调：esp_lvgl_port 自带的实现忽略 draw_bitmap 的返回值，
 * 刷屏失败时不会调 lv_display_flush_ready()，单缓冲下 LVGL 会死等（界面永久卡死） */
lv_display_set_flush_cb(disp, periph_lcd_flush_cb);
```

`periph_lcd_flush_cb` 里做 RGB565 字节交换（LVGL 9 已移除 `LV_COLOR_16_SWAP`），并在写入失败时补一次 `lv_display_flush_ready()`。

```c
/* periph_touch_init() 内：注册只读缓存的 POINTER input device */
lv_indev_t *indev = lv_indev_create();
lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_read_cb(indev, touch_indev_read_cb);   // 读取 periph_touch 缓存的坐标
lv_indev_set_display(indev, periph_lcd_get_disp());
```

不使用 `lvgl_port_add_touch()`：触摸设备的轮询由 `periph_touch` 的扫描任务独占，以避免 LVGL 与触摸任务并发读取同一 `esp_lcd_touch` 句柄造成丢点。

### 10.2 线程安全

- 所有 LVGL API 调用必须经 `lvgl_port_lock(0) / unlock()`
- LVGL tick 由 esp_lvgl_port 自动处理
- 长操作（音频解码、文件读取）放在其他任务，通过队列投递给 LVGL 任务

## 11. 输入路由

| 操作 | 默认路由 |
|------|----------|
| 触摸屏幕 | 当前界面（经 LVGL input device） |
| 状态栏按钮 | 返回、主页 |
| BOOT 键单击 | 全局 → `fw_app_mgr_back()` |
| BOOT 键双击 | 全局 → `fw_app_mgr_back_to_home()` |
| BOOT 键长按 | 全局 → 电源菜单 |

不使用滑动手势：导航全部由状态栏按钮完成，避免手势与应用内滑动冲突。

## 12. UI 视觉规范（新增 / 修改界面必须遵守）

后续所有界面都按下表执行，与现有界面保持同一套观感；不要另起一套风格。单位均为 LVGL 像素（屏幕 320×240，状态栏 28 px，应用内容区 320×212）。

### 12.1 尺寸与间距

| 元素 | 规格 |
|------|------|
| 状态栏 | 高 28（`FW_STATUSBAR_H`），底部 1 px 线 |
| 状态栏按钮 | 32 × 20，另加 `lv_obj_set_ext_click_area(btn, 8)` 扩大触摸区（视觉尺寸不变） |
| 状态栏位置 | 左：返回 x=8、主页 x=48；右：状态图标从右往左排 |
| 页面内边距 | 12（内容区 `pad_all`），行间距 8（`pad_row`） |
| 入口卡片 / 列表项 | 高 50 / 高 38 |
| 桌面格子 | 70 × 86，4 列，列间距 8，网格整体上边距 6 |
| 对话框 | 面板 268 × 156；正文宽 244；按钮行 244 × 40 贴底；按钮 88 × 34 |
| Toast | 宽 272，距底部 44 |
| 通用组件 | 进度条面板 240 × 72；列表容器内边距 8、行距 4 |

### 12.2 圆角与描边

| 元素 | 圆角 | 描边 |
|------|------|------|
| 卡片 / 入口行 | 10 | 1 px `border` |
| 小按钮 / 列表项 | 6 ~ 8 | 1 px `border` |
| 桌面格子 / 对话框面板 | 12 | 1 px `border` |
| 状态栏 / 页面 | 0 | 状态栏底部画 1 px |

阴影一律 `shadow_width = 0`：小屏低分辨率下阴影只会发糊。对话框用全屏 50% 黑遮罩，模态到底。

### 12.3 配色使用规则

- 页面底 `bg_primary`；状态栏 `bg_secondary`；卡片、按钮、列表、Toast、对话框 `bg_card`
- 正文 `text_primary`；说明与次要信息 `text_secondary`；禁用态 `text_disabled`
- 主色 `accent` 只用于：图标、滑块指示条与圆点、主按钮（如对话框"确定"）、状态栏播放图标
- 分隔线 `divider` 用于列表内分隔与桌面空槽描边；`border` 用于卡片 / 按钮 / 描边与状态栏底线
- 语义色 `success` / `warning` / `error` 只用于状态提示与告警
- 两套主题都必须完整可用：控件一律显式设色（见 3.1），不依赖 LVGL 自带主题

### 12.4 字体与图标

- 中文正文 14 px（`fw_asset_font_cn()`），标题 16 px（`fw_asset_font_cn_large()`）
- 拉丁与数字：`fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()`
- 图标尺寸：状态栏 14，卡片与列表 20，桌面 24；优先用 LVGL 内置 `LV_SYMBOL_*`
- 界面文案的汉字必须在字体子集内（见 5.1），改完跑 `python tools/check_cn_text.py`

### 12.5 交互约定

- "点击"统一 `LV_EVENT_SHORT_CLICKED`；滑块 / 复选框用 `LV_EVENT_VALUE_CHANGED`；长按用 `LV_EVENT_LONG_PRESSED`
- 不使用滑动手势，也不做应用内标题栏：页内返回交给状态栏返回键（App 实现 `on_back`），主页交给状态栏主页键
- 全局浮层挂在 `lv_layer_top()`，并确保 `lv_layer_top()` 本身不吞触摸（见 AGENTS 4.1）
- 屏幕切换动画统一 `LV_SCR_LOAD_ANIM_FADE_IN` + 250 ms；主题重建用 `LV_SCR_LOAD_ANIM_NONE`
- 反馈统一 `fw_ui_toast()`（1500 ~ 2500 ms），确认类操作统一 `fw_ui_dialog()`
- 所有文案遵循 AGENTS 的术语约定（`Wi-Fi`、`I2C`、`SPIFFS`、数字与单位间加空格等）

### 12.6 新增界面自检清单

1. 颜色只从 `fw_theme_color_*()` 取，不依赖 LVGL 自带主题
2. 卡片 / 按钮 / 浮层都有 1 px `border`，`shadow_width = 0`
3. 点击用 `SHORT_CLICKED`，滑块 / 复选框用 `VALUE_CHANGED`
4. 文字走 `fw_asset_font_*`；新汉字加进 `tools/cn_chars.py` 的 `CN_CHARS` 后重新生成字体
5. 深浅两个主题各看一遍（显示 App → 主题），确认没有看不见的控件、没有糊成一团的层次
6. 跑 `python tools/check_ui_colors.py` 与 `python tools/check_cn_text.py`
