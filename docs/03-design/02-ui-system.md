# UI 系统详细设计

UI 系统基于 LVGL 9 + esp_lvgl_port 2.x，由 Framework 层统一封装。

新增或修改界面前，先读第 12 节《UI 视觉规范》：尺寸、圆角、间距、配色令牌与交互按那一节执行，保持与现有界面同一套观感。

## 1. UI 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                 LVGL Display 320×240                      │
├──────────────────────────────────────────────────────────┤
│ [返回][主页]   10:30    [摄][卡][录][亮][蓝牙][声][Wi-Fi]  │  ← fw_statusbar (28 px)
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
- 开机时状态栏随浮层一起隐藏：`fw_init()` 在创建状态栏**之前**就把 `lv_layer_top()` 藏起来，默认屏底色是纯黑，因此背光点亮后到 Logo 上屏之间只显示黑屏，不会先闪出状态栏或桌面元素（见 AGENTS 4.27）

## 2. fw_statusbar（状态栏）

状态栏是唯一的常驻浮层，同时承担全局导航与状态显示。下面的布局、图标语义、触发条件都是**固定设计**：改状态栏前先改本节，再改代码。

### 2.1 布局

```
┌────────────────────────────────────────────────────┐
│ [返回][主页]   10:30   [Wi-Fi][蓝牙][声音][录音][摄像头][TF 卡] │
└────────────────────────────────────────────────────┘
   左：返回 / 主页    中：时间（偏左）    右：状态图标（常显）
```

| 元素 | 规格 |
|------|------|
| 状态栏 | 高 28（`FW_STATUSBAR_H`），底色 `bg_secondary`，底部 1 px `border` 线 |
| 返回 / 主页按钮 | 32 × 20，`bg_card` + 圆角 6，`LV_ALIGN_LEFT_MID`，`x = 8` / `48`，`lv_obj_set_ext_click_area(btn, 8)` 扩大触摸区（视觉尺寸不变） |
| 时间 | `LV_ALIGN_CENTER` + `x = -32`（左按钮组与右图标组之间可用区的中点约 126 px），`text_primary`；未同步显示 `--:--` |
| 状态图标 | 6 个，20 × 20，`LV_ALIGN_RIGHT_MID`，`x = -128 / -104 / -80 / -56 / -32 / -8`（右边缘间距 24 px），全部常显 |

间距校验：左按钮组右缘 80 px，最靠左的 Wi-Fi 图标左缘 172 px；时钟最宽 `23:59` 约 39 px、右缘约 148 px，两侧各留 24 px 以上，320 px 下不重叠。

### 2.2 状态图标语义（固定）

六个图标**全部常显、都不接受点击**（导航只由左侧两个按钮承担）。统一配色：高亮 `accent`，置灰 `text_disabled`，不存在第二种高亮色。每个图标只绑定**一个**状态：

| 图标 | 资源 | 绑定状态 | 高亮条件 | 置灰条件 | 即时更新来源 |
|------|------|----------|----------|----------|--------------|
| Wi-Fi | `icon_status_wifi` | 已连上 AP | `svc_net_get_status().wifi_connected` 为真 | 未连接 / 连接中 / 连接失败 | `SVC_EVENT_WIFI_CONNECTED`、`SVC_EVENT_WIFI_DISCONNECTED` |
| 蓝牙 | `icon_status_bt` | 广播中或已连接 | `svc_bt_get_state()` 为 `ADVERTISING` 或 `CONNECTED` | `OFF`、`READY`（协议栈就绪但未广播） | `SVC_EVENT_BT_STATE_CHANGED` |
| 声音 | `icon_status_sound` | 扬声器在放音 | `svc_audio_get_state()` 为 `PLAYING` | `IDLE`、`PAUSED`、`RECORDING` | `SVC_EVENT_AUDIO_PLAYBACK_STARTED` / `FINISHED` |
| 录音 | `icon_status_record` | 麦克风在收音 | `svc_audio_get_state()` 为 `RECORDING` | 非录音状态 | `SVC_EVENT_AUDIO_RECORD_STARTED` / `FINISHED` |
| 摄像头 | `icon_status_camera` | 摄像头已打开 | `svc_camera_is_open()` 为真 | 未打开 / 打开失败 | `SVC_EVENT_CAMERA_STATE_CHANGED`（负载 `bool`） |
| TF 卡 | `icon_status_sd` | 卡可用（已挂载且仍能访问） | 挂载成功 | 无卡 / 挂载失败 / 卡不可访问 | `SVC_EVENT_SD_MOUNTED`、`SVC_EVENT_SD_UNMOUNTED` |

固定约定：

- 声音 / 录音绑定的就是 `svc_audio` 的状态机。系统里所有放音与收音都经 `svc_audio`（App、脚本、开机提示音），所以 `PLAYING` / `RECORDING` 等价于"扬声器 / 麦克风正在被调用"；暂停（`PAUSED`）不算在用。
- 蓝牙只认 `ADVERTISING` / `CONNECTED`；`READY` 表示协议栈已就绪但用户把广播关了，必须置灰。
- Wi-Fi 只看连接与否，信号强度（`fw_statusbar_set_wifi()` 的 `rssi` 形参）目前不用。
- TF 卡的"可用"由 `svc_storage` 的热插拔轮询判定：拔出约 1 s 内反映，插入最长约 10 s（没卡时每 10 次轮询才尝试挂载一次，避免 IDF 挂载失败日志每秒刷屏）。**这是有意为之，不要改成秒级。**
- 每秒兜底同步（见 2.4）只是保险；正常路径都由上表的即时事件驱动。

### 2.3 接口

```c
esp_err_t fw_statusbar_init(void);
esp_err_t fw_statusbar_rebuild(void);

esp_err_t fw_statusbar_set_wifi(int8_t rssi, bool connected);
esp_err_t fw_statusbar_set_sound_playing(bool on);
esp_err_t fw_statusbar_set_bluetooth(bool on);
```

只导出这三个 setter（供事件处理器复用）；录音、摄像头、TF 卡图标由状态栏内部订阅事件维护，不导出 setter。

### 2.4 实现要点

- 图标资源是白色 + alpha 的 20×20 自绘图（造型见 5.3），运行时用 `lv_obj_set_style_image_recolor()` + `recolor_opa = LV_OPA_COVER` 染色。`icon_set_on()` 在颜色没变时直接返回，所以每秒兜底不会造成无谓重绘。
- 深色 / 浅色主题共用同一份图标资源：换主题时 `fw_statusbar_rebuild()` 重建控件，`statusbar_build()` 末尾按真实状态重新套一遍（`svc_net_get_status()` / `svc_bt_get_state()` / `svc_audio_get_state()` / `svc_camera_is_open()` / `svc_storage_get_info()`），保证订阅之前发生的状态变化不会漏。
- 1 s `lv_timer` 做两件事：刷新时间；把 Wi-Fi / 蓝牙 / 声音 / 录音 / 摄像头五个图标按真实状态重新同步一次（兜底）。TF 卡不在轮询里（查容量要访问文件系统，代价大）。
- 订阅清单：`SVC_EVENT_TIME_SYNCED` / `TIME_CHANGED` / `TIMEZONE_CHANGED`、`WIFI_CONNECTED` / `WIFI_DISCONNECTED`、`BT_STATE_CHANGED`、`AUDIO_PLAYBACK_STARTED` / `FINISHED`、`AUDIO_RECORD_STARTED` / `FINISHED`、`CAMERA_STATE_CHANGED`、`SD_MOUNTED` / `SD_UNMOUNTED`。
- 左侧按钮：返回 → `fw_app_mgr_back()`（先询问当前 App 的 `on_back()`），主页 → `fw_app_mgr_back_to_home()`。
- 状态栏高度由 `FW_STATUSBAR_H` 定义；App 内容区与浮层都以此为顶部偏移。

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

- 状态栏图标：6 个自绘的 20×20 图标（Wi-Fi / 蓝牙 / 声音 / 录音 / 摄像头 / TF 卡），
  白色图形 + alpha，运行时用 `image_recolor` 染色；绑定语义见第 2.2 节，造型见 5.3
- 卡片与列表内的功能性图标：LVGL 内置 symbol（`LV_SYMBOL_LEFT` / `LV_SYMBOL_HOME` 等）
- 桌面 App 图标：自绘彩色图标（见 5.3）

### 5.3 图标资源

两组自绘图标都由 Pillow 脚本生成（4 倍超采样后 LANCZOS 缩小），RGB565 按小端存放：

| 资源 | 尺寸 / 格式 | 生成脚本 | 用途 |
|------|-------------|----------|------|
| `icons_home.c` / `fw_home_icons.h` | 40×40，RGB565A8，彩色 | `tools/gen_home_icons.py` | 桌面 22 个 App 彩色图标 |
| `icons_status.c` / `fw_status_icons.h` | 20×20，RGB565A8，白色 + alpha | `tools/gen_status_icons.py` | 状态栏 6 个图标（运行时染色） |

桌面图标由 App 描述符的 `icon_64` 指向；状态栏图标由 `fw_statusbar` 直接用
`icon_status_*`。两组资源的生成脚本都能单独重跑，改完不要手工改字节序。

状态栏 6 个图标的造型（固定，从 `gen_status_icons.py` 里改完重跑）：

| 图标 | 造型 |
|------|------|
| Wi-Fi | 三道弧 + 底部圆点 |
| 蓝牙 | 蓝牙符文：竖线 + 上下两个折角 |
| 声音 | 喇叭（梯形箱体 + 锥面）+ 两道声波弧 |
| 录音 | 麦克风：胶囊拾音头 + 半圆托架 + 支杆与底座 |
| 摄像头 | 相机机身（圆角矩形）+ 顶部取景器凸起 + 镜头环（环心留高光点） |
| TF 卡 | 缺角卡形 + 三条触点槽 |

## 6. 桌面

桌面是"Home"这个特殊 App：

```c
// main/apps/app_home/app_home.c
const fw_app_desc_t app_home_desc = {
    .name = "Home",
    .title = "桌面",
    .icon_64 = NULL,
    .symbol = LV_SYMBOL_HOME,
    .on_create = home_on_create,
    .on_destroy = home_on_destroy,
};
```

桌面内容区：
- 一屏 4×2 = 8 个应用格子（`lv_tileview`，超出可左右滑动翻页），顺序即 App 注册顺序
  （与原始需求「内置应用」列表一致，见 `app_register.c`）
- 每个格子 73×76：上方 40×40 彩色图标（描述符 `icon_64`），下方中文名称（描述符 `title`）居中显示
- 名称只占一行：宽度按"4 个汉字 + `...`"预留（约 71 px），放不下由 `LV_LABEL_LONG_MODE_DOTS` 换成 `...`
- 网格内边距与列间距都是 5 px，整体在内容区里垂直居中
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
  3. 解除静音后等功放稳定，再播一声 1 kHz / 300 ms 提示音（全程约 2.05 s）
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
| 状态栏图标 | 时间、Wi-Fi、蓝牙、声音、录音、摄像头、TF 卡（见第 2 节） |

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
    .rotation = {                  // 与 drv_st7789 设的面板方向一致（已交换 XY + 镜像 X）
        .swap_xy = true,
        .mirror_x = true,
        .mirror_y = false,
    },
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
| 状态栏位置 | 左：返回 x=8、主页 x=48；中：时间 `LV_ALIGN_CENTER` x=-32；右：状态图标从左往右排（Wi-Fi -128、蓝牙 -104、声音 -80、录音 -56、摄像头 -32、TF 卡 -8），间距 24，全部常显（权威规格与语义见第 2 节） |
| 页面内边距 | 12（内容区 `pad_all`），行间距 8（`pad_row`） |
| 入口卡片 / 列表项 | 高 50 / 高 38 |
| 桌面格子 | 73 × 76，4 列，列间距与网格内边距均为 5，网格垂直居中 |
| 对话框 | 面板 268 × 156；正文宽 244；按钮行 240 × 36 贴底；按钮在行内均分（间距 8） |
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
- 主色 `accent` 只用于：图标、滑块指示条与圆点、主按钮（如对话框"确定"）、状态栏高亮图标
- 分隔线 `divider` 用于列表内分隔与桌面空槽描边；`border` 用于卡片 / 按钮 / 描边与状态栏底线
- 语义色 `success` / `warning` / `error` 只用于状态提示与告警
- 两套主题都必须完整可用：控件一律显式设色（见 3.1），不依赖 LVGL 自带主题

### 12.4 字体与图标

- 中文正文 14 px（`fw_asset_font_cn()`），标题 16 px（`fw_asset_font_cn_large()`）
- 拉丁与数字：`fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()`
- 图标尺寸：状态栏 20×20（自绘，染色），桌面 40×40（自绘彩色），卡片与列表内的功能性图标 20（LVGL 内置 `LV_SYMBOL_*`）
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
