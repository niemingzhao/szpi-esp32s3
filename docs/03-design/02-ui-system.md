# UI 系统详细设计

UI 系统基于 LVGL 9 + esp_lvgl_port 2.x，由 Framework 层统一封装。

新增或修改界面前，先读第 12 节《UI 视觉规范》：尺寸、圆角、间距、配色令牌与交互按那一节执行，保持与现有界面同一套观感。

## 1. UI 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                 LVGL Display 320×240                      │
├──────────────────────────────────────────────────────────┤
│ [返回][主页]   10:30    [Wi-Fi][蓝牙][声音][录音][摄像头][TF 卡]  │  ← fw_statusbar (28 px)
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
- 没有底部虚拟按键栏；除桌面的左右翻页（lv_tileview）外不使用滑动手势
- 开机时状态栏随浮层一起隐藏：`fw_init()` 在创建状态栏**之前**就把 `lv_layer_top()` 藏起来，默认屏底色是纯黑，因此背光点亮后到 Logo 上屏之间只显示黑屏，不会先闪出状态栏或桌面元素

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
| 时间 | `LV_ALIGN_CENTER` + `x = -32`（左按钮组与右图标组之间可用区的中点约 126 px），`text_primary`；未同步显示 `--:--`；12 / 24 小时制跟随 `svc_time_get_24h()`（与时钟 App 共用 `sys/clock_24h`） |
| 状态图标 | 6 个，20 × 20，`LV_ALIGN_RIGHT_MID`，`x = -128 / -104 / -80 / -56 / -32 / -8`（右边缘间距 24 px），全部常显 |

间距校验：左按钮组右缘 80 px，最靠左的 Wi-Fi 图标左缘 172 px；时钟最宽 `23:59` 约 39 px、右缘约 148 px，两侧各留 24 px 以上，320 px 下不重叠。

### 2.2 状态图标语义（固定）

六个图标**全部常显、都不接受点击**（导航只由左侧两个按钮承担）。统一配色：高亮 `accent`，置灰 `text_disabled`，不存在第二种高亮色。每个图标只绑定**一个**状态：

| 图标 | 资源 | 绑定状态 | 高亮条件 | 置灰条件 | 即时更新来源 |
|------|------|----------|----------|----------|--------------|
| Wi-Fi | `icon_status_wifi` | 已连上 AP | `svc_net_get_status().wifi_connected` 为真 | 未连接 / 连接中 / 连接失败 | `SVC_EVENT_WIFI_CONNECTED`、`SVC_EVENT_WIFI_DISCONNECTED` |
| 蓝牙 | `icon_status_bt` | 广播中 / 从机链路已连接 / 中心链路已连接 | `svc_bt_get_state()` 为 `ADVERTISING` 或 `CONNECTED`，或 `svc_bt_central_is_connected()` 为真（两种角色任一生效就高亮） | 都没连（`OFF`、`READY`，且中心未连接） | `SVC_EVENT_BT_STATE_CHANGED`；中心链路的连接 / 断开不发事件，靠 1 s 兜底轮询 |
| 声音 | `icon_status_sound` | 扬声器在放音 | `svc_audio_get_state()` 为 `PLAYING` | `IDLE`、`PAUSED`、`RECORDING` | `SVC_EVENT_AUDIO_PLAYBACK_STARTED` / `FINISHED` |
| 录音 | `icon_status_record` | 麦克风在收音 | `svc_audio_get_state()` 为 `RECORDING` | 非录音状态 | `SVC_EVENT_AUDIO_RECORD_STARTED` / `FINISHED` |
| 摄像头 | `icon_status_camera` | 摄像头已打开 | `svc_camera_is_open()` 为真 | 未打开 / 打开失败 | `SVC_EVENT_CAMERA_STATE_CHANGED`（负载 `bool`） |
| TF 卡 | `icon_status_sd` | 卡可用（已挂载且仍能访问） | 挂载成功 | 无卡 / 挂载失败 / 卡不可访问 | `SVC_EVENT_SD_MOUNTED`、`SVC_EVENT_SD_UNMOUNTED` |

固定约定：

- 声音 / 录音绑定的就是 `svc_audio` 的状态机。系统里所有放音与收音都经 `svc_audio`（App、脚本、开机提示音），所以 `PLAYING` / `RECORDING` 等价于"扬声器 / 麦克风正在被调用"；暂停（`PAUSED`）不算在用。
- 蓝牙只认 `ADVERTISING` / `CONNECTED`；`READY` 表示协议栈已就绪但用户把广播关了，必须置灰。
- Wi-Fi 只看连接与否，信号强度目前不用（所以接口里没有 `rssi`）。
- TF 卡的"可用"由 `svc_storage` 的热插拔轮询判定：拔出约 1 s 内反映，插入最长约 10 s（没卡时每 10 次轮询才尝试挂载一次，避免 IDF 挂载失败日志每秒刷屏）。**这是有意为之，不要改成秒级。**
- 每秒兜底同步（见 2.4）只是保险；正常路径都由上表的即时事件驱动。

### 2.3 接口

```c
esp_err_t fw_statusbar_init(void);
esp_err_t fw_statusbar_rebuild(void);
```

对外只有这两个接口：所有图标都由状态栏内部订阅上面那张表里的事件维护（另有 1 s 兜底同步），不对外暴露 setter。

### 2.4 实现要点

- 图标资源是白色 + alpha 的 20×20 自绘图（造型见 5.3），运行时用 `lv_obj_set_style_image_recolor()` + `recolor_opa = LV_OPA_COVER` 染色。`icon_set_on()` 在颜色没变时直接返回，所以每秒兜底不会造成无谓重绘。
- 深色 / 浅色主题共用同一份图标资源：换主题时 `fw_statusbar_rebuild()` 重建控件，`statusbar_build()` 末尾按真实状态重新套一遍（`svc_net_get_status()` / `svc_bt_get_state()` / `svc_audio_get_state()` / `svc_camera_is_open()` / `svc_storage_get_info()`），保证订阅之前发生的状态变化不会漏。
- 1 s `lv_timer` 做两件事：刷新时间；把 Wi-Fi / 蓝牙 / 声音 / 录音 / 摄像头五个图标按真实状态重新同步一次（兜底）。TF 卡不在轮询里（查容量要访问文件系统，代价大）。
- 订阅清单：`SVC_EVENT_TIME_SYNCED` / `TIME_CHANGED` / `TIMEZONE_CHANGED`、`WIFI_CONNECTED` / `WIFI_DISCONNECTED`、`BT_STATE_CHANGED`、`AUDIO_PLAYBACK_STARTED` / `FINISHED`、`AUDIO_RECORD_STARTED` / `FINISHED`、`CAMERA_STATE_CHANGED`、`SD_MOUNTED` / `SD_UNMOUNTED`。
- 左侧按钮：返回（`icon_ui_back` 左箭头）→ `fw_app_mgr_back()`（先询问当前 App 的 `on_back()`），主页（`icon_ui_home` 房子）→ `fw_app_mgr_back_to_home()`。这两个图标是状态栏专用的；日历的月份 / 年份箭头另用 `icon_ui_left` / `icon_ui_right` 系列 chevron。
- 状态栏高度由 `FW_STATUSBAR_H` 定义；App 内容区与浮层都以此为顶部偏移。

## 3. 主题系统

### 3.1 调色板

深色 / 浅色两套色板，取色统一走 `fw_theme_color_*()`，不要在界面代码里写死颜色。

| 令牌 | 深色 | 浅色 | 用途 |
|------|------|------|------|
| `bg_primary` | `0x0A0D12` | `0xE4ECF7` | 页面底色 |
| `bg_secondary` | `0x151A22` | `0xFFFFFF` | 状态栏 / 浮层 / 列表项 |
| `bg_card` | `0x1F2630` | `0xFFFFFF` | 卡片 / 按钮 |
| `text_primary` | `0xFFFFFF` | `0x0F1319` | 正文 |
| `text_secondary` | `0xC4CDDB` | `0x4C5768` | 次要文字 |
| `text_disabled` | `0x606A78` | `0x98A3B2` | 禁用态 |
| `accent` | `0x3D8CFF` | `0x0A5CD6` | 主色（图标 / 滑块指示条 / 主按钮 / 主信息卡描边） |
| `accent2` | `0xFF6B3D` | `0xE8551F` | 次色 |
| `success` | `0x4ADE80` | `0x16A34A` | 成功 |
| `warning` | `0xFFC94D` | `0xD97706` | 警告 |
| `error` | `0xFF5A5C` | `0xDC2626` | 错误 |
| `divider` | `0x2A323D` | `0xD6E0EC` | 分隔线（设置行下线 / 列表项描边） |
| `border` | `0x333D4A` | `0xBCC9DA` | 卡片 / 状态栏描边 |

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

`fw_theme_apply()` 会同步切换 LVGL 自带主题的明暗，并发布 `SVC_EVENT_THEME_CHANGED`；框架收到后重建状态栏、浮层以及**当前前台 App** 的界面（其余 App 标记待重建、下次显示前重建），因此**立即全局生效**（界面回到初始页）。

## 4. 通用 UI 组件

封装在 Framework 层：

| 组件 | 用途 |
|------|------|
| 进度条 | 长操作反馈（下载 / 文件复制 / 加载） |
| 对话框 | 二次确认 / 警告 / 信息提示 |
| Toast | 短提示（自动消失） |
| 列表 | 设置项 / 文件浏览 |
| 网格 | 应用图标 / 图片缩略图 |
| 详情格 | 只读规格 / 数值的九宫格（`fw_ui_table()`，天气实况 / 性能监控 / 关于本机） |
| 页面 / 整行入口 / 滑块行 / 底部动作按钮 | `fw_ui_page()` / `fw_ui_row_btn*()` / `fw_ui_slider_row()` / `fw_ui_action_btn()`，界面页面的标准骨架（规格见 12.1） |

Toast 与对话框同一时刻只保留一个：新的 Toast 会顶掉旧的（不然连续点几下会叠成一摞）。两者的句柄都在对象的 `LV_EVENT_DELETE` 回调里清掉，连带挂在对象上的定时器一起删 —— 不要用 `lv_obj_is_valid()` 去判断"对象还在不在"。

每个组件提供统一 API：

```c
lv_obj_t *fw_ui_progress_bar(lv_obj_t *parent, const char *title);
esp_err_t fw_ui_progress_set(lv_obj_t *bar, uint8_t percent);
esp_err_t fw_ui_progress_title(lv_obj_t *bar, const char *text);

lv_obj_t *fw_ui_dialog(lv_obj_t *parent, const char *title, const char *msg,
                       fw_dialog_btn_t buttons, fw_dialog_cb_t cb, void *user);
esp_err_t fw_ui_dialog_close(lv_obj_t *dlg);

lv_obj_t *fw_ui_toast(const char *msg, uint32_t duration_ms);

lv_obj_t *fw_ui_list(lv_obj_t *parent, const char *title);
lv_obj_t *fw_ui_list_add(lv_obj_t *list, const char *text, lv_event_cb_t cb, void *user);
lv_obj_t *fw_ui_grid(lv_obj_t *parent, uint8_t cols, int32_t item_w, int32_t item_h);
```

详情格（只读规格 / 数值的九宫格）与几个显示用的公共件：

```c
lv_obj_t *fw_ui_table(lv_obj_t *parent, uint8_t cols, uint8_t rows, const char *const *labels);   /* 一格 22 px 高，标题 + 数值 */
esp_err_t fw_ui_table_value(lv_obj_t *table, uint8_t index, const char *value);     /* 索引 = 行 × 列数 + 列 */
esp_err_t fw_ui_table_value_scroll(lv_obj_t *table, uint8_t index, bool on);        /* 该格数值超长时横向滚动 */
void fw_ui_format_size(char *buf, size_t len, uint64_t bytes);                      /* "512 B" / "24 KB" / "7.9 MB" / "1.2 GB" */
lv_obj_t *fw_ui_action_btn(lv_obj_t *parent, const lv_image_dsc_t *icon, const char *text,
                           int32_t w, lv_event_cb_t cb, void *user);                 /* 底部动作条的大按钮（高 44） */
esp_err_t fw_ui_list_item_value(lv_obj_t *item, const char *value);                 /* 改列表项右侧数值 */
```

`fw_ui_table()` 的列数按内容定：3 列一格约 90 px，只放得下短值（`12%`）；"内部内存 + 45 KB"
这类用 2 列（一格约 140 px）。长文本要用整行入口 `fw_ui_row_btn*()` 的数值位（超长用省略号截断）。
`fw_ui_action_btn()` 建的按钮里，图标是第 0 个子对象、文字是第 1 个，播放⇄暂停、开始⇄停止这类切换直接取出来改。

## 5. 字体与图标资源

### 5.1 字体

- 英文 / 数字：LVGL 内置 Montserrat 14（正文）、20（中号）、24（大号）、32（大数值）
- 中文：Noto Sans SC 栅格化的 **14 px（正文）** 与 **16 px（标题）**，覆盖 GB2312 全集（6763 汉字 + 682 符号），位于 `main/framework/assets/fw_fonts.c`
- 由 `tools/gen_fw_fonts.py` 生成（LVGL 9 字体格式）；另有 GBK 全集（20902 汉字）的 14 px 回退字体 `font_cn_extra`，再回退 Montserrat 14
- 所有字体统一经 `fw_asset_font_cn()` / `fw_asset_font_cn_large()` / `fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()` / `fw_asset_font_32()` 获取

### 5.2 图标

- 状态栏图标：6 个自绘的 20×20 图标（Wi-Fi / 蓝牙 / 声音 / 录音 / 摄像头 / TF 卡），
  白色图形 + alpha，运行时用 `image_recolor` 染色；绑定语义见第 2.2 节，造型见 5.3
- 卡片与列表内的功能性图标：自绘的 20×20 `icon_ui_*`（见 5.3）；还没改造的 App 暂时仍用 LVGL 内置 symbol
- 桌面 App 图标：自绘彩色图标（见 5.3）

### 5.3 图标资源

三组自绘图标都由 `tools/gen_fw_icons.py` 生成（4 倍超采样后 LANCZOS 缩小），集中在
`main/framework/assets/fw_icons.c`，声明在 `include/fw_icons.h`，RGB565 按小端存放：

| 资源 | 尺寸 / 格式 | 用途 |
|------|-------------|------|
| `icon_ui_*` | 20×20，RGB565A8，白色 + alpha | 界面功能图标（头部按钮 / 行入口 / 月份箭头等，运行时染色） |
| `icon_home_*` | 40×40，RGB565A8，彩色 | 桌面 22 个 App 彩色图标 |
| `icon_status_*` | 20×20，RGB565A8，白色 + alpha | 状态栏 6 个图标（运行时染色） |

桌面图标由 App 描述符的 `icon` 指向；状态栏图标由 `fw_statusbar` 直接用
`icon_status_*`。生成完不要手工改字节序（预览图在 `tools/icons_*_preview.png`）。

状态栏 6 个图标的造型（固定，从 `tools/gen_fw_icons.py` 里改完重跑）：

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
    .icon = NULL,
    .on_create = home_on_create,
    .on_destroy = home_on_destroy,
};
```

桌面内容区：
- 一屏 4×2 = 8 个应用格子（`lv_tileview`，超出可左右滑动翻页），顺序即 App 注册顺序
  （见 `app_common.c`）
- 每个格子 73×76：上方 40×40 彩色图标（描述符 `icon`），下方中文名称（描述符 `title`）居中显示
- 名称只占一行：宽度按"4 个汉字 + `...`"预留（约 71 px），放不下由 `LV_LABEL_LONG_MODE_DOTS` 换成 `...`
- 网格内边距与列间距 5 px、行间距 10 px，整体在内容区里垂直居中
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
        .buff_spiram = false,      // 绘制缓冲放内置 DMA 内存（SPI 不能直接 DMA PSRAM）
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
| 状态栏按钮 | 32 × 20，卡片底 + 1 px `border` + 圆角 6，另加 `lv_obj_set_ext_click_area(btn, 8)` 扩大触摸区（视觉尺寸不变）；浅色主题下状态栏与按钮同为白，靠描边区分 |
| 状态栏位置 | 左：返回 x=8、主页 x=48；中：时间 `LV_ALIGN_CENTER` x=-32；右：状态图标从左往右排（Wi-Fi -128、蓝牙 -104、声音 -80、录音 -56、摄像头 -32、TF 卡 -8），间距 24，全部常显（权威规格与语义见第 2 节） |
| 页面内边距 | 12（内容区 `pad_all`），行间距 8（`pad_row`） |
| 设置行 / 列表项 | 高 44 / 高 40（设置行两两一组放进 `fw_ui_group()` 卡片里） |
| 内容卡片（含主信息卡） | 高 60 ~ 70；主信息卡 2 px 主色描边 |
| 桌面格子 | 73 × 76，4 列，列间距与网格内边距 5、行间距 10，网格垂直居中 |
| 对话框 | 面板宽 268、高随内容（标题 + 正文 + 按钮行，上限 228，不会压到按钮上）；正文宽 244；按钮行高 36；按钮在行内均分（间距 8） |
| Toast | 宽 272，距底部 44 |
| 通用组件 | 进度条面板 240 × 72；列表容器内边距 8、行距 4 |

### 12.2 圆角与描边

| 元素 | 圆角 | 描边 |
|------|------|------|
| 卡片（内容块 / 列表容器 / 对话框） | 12 | 1 px `border` |
| 主信息卡（页面主角：天气实况 / 当前网络） | 12 | **2 px `accent`** |
| 分组卡片（包住若干设置行） | 12 | 1 px `border` |
| 设置行（卡片内的行） | 0 | 只画底部 1 px `divider`（行之间分隔） |
| 列表项 / 小按钮 | 8 | 1 px `divider`（列表项）/ `border`（按钮） |
| 桌面格子 / 对话框面板 | 12 | 1 px `border` |
| 状态栏 / 页面 | 0 | 状态栏底部画 1 px |

阴影一律 `shadow_width = 0`：小屏低分辨率下阴影只会发糊。对话框用全屏 50% 黑遮罩，模态到底。

### 12.3 配色使用规则

- 页面底 `bg_primary`；状态栏 `bg_secondary`；卡片、按钮、列表、Toast、对话框 `bg_card`
- 正文 `text_primary`；说明与次要信息 `text_secondary`；禁用态 `text_disabled`
- 主色 `accent` 只用于：图标、滑块指示条与圆点、主按钮（如对话框"确定"）、状态栏高亮图标
- 辅色 `accent2` 是第二强调色，鼓励按需多用（日历的周末日期就用它）；但同一屏里主色 / 辅色别互相抢，语义色仍只用于状态提示与告警
- 分隔线 `divider` 用于列表内分隔与桌面空槽描边；`border` 用于卡片 / 按钮 / 描边与状态栏底线
- 语义色 `success` / `warning` / `error` 只用于状态提示与告警
- **选中项**（标签页、列表 / 网格的当前项）统一用**主色描边 + 主色文字**，不用主色填充 —— 主色填充留给主按钮。未选中用该控件的常规描边色（卡片 / 按钮 / 芯片 `border`，列表行是底部 `divider`）。`fw_ui_list_mark()` 是列表场景的参考实现
- 两套主题都必须完整可用：控件一律显式设色（见 3.1），不依赖 LVGL 自带主题

### 12.4 字体与图标

- 中文正文 14 px（`fw_asset_font_cn()`），标题 16 px（`fw_asset_font_cn_large()`）
- 拉丁与数字：`fw_asset_font_14()` / `fw_asset_font_20()` / `fw_asset_font_24()` / `fw_asset_font_32()`
- 图标尺寸：状态栏状态图标 20×20（`icon_status_*`）、桌面 40×40（`icon_home_*`）、界面功能图标 20×20（`icon_ui_*`）；状态栏与功能图标是白色 + alpha、运行时用 `image_recolor` 染色，桌面图标是彩色、不染色
- App 界面里的功能图标统一用 `fw_icons.h` 的 `icon_ui_*`：行入口 `fw_ui_row_btn_img()`、头部动作按钮 `fw_ui_icon_btn()`、单独摆放 `fw_ui_icon()`（强调色；未启用/无状态用 `text_disabled`）。**不要再在 App 里用 `LV_SYMBOL_*` 画图标**（只为还没改造的 App 与键盘内部保留）
- 图标资源由 `python tools/gen_fw_icons.py` 生成（预览图 `tools/icons_ui_preview.png` 等），新增 / 改动图标改脚本后重新生成，不要手改 `fw_icons.c`
- 界面文案的汉字必须在字体覆盖范围内（见 5.1），改完跑 `python tools/check_cn_text.py`

### 12.5 交互约定

- "点击"统一 `LV_EVENT_SHORT_CLICKED`；滑块 / 复选框用 `LV_EVENT_VALUE_CHANGED`；长按用 `LV_EVENT_LONG_PRESSED`（需要长按连发的按钮用 `LV_EVENT_LONG_PRESSED_REPEAT`，如键鼠页的方向键 / 鼠标移动）
- 除桌面翻页外不使用滑动手势，也不做应用内标题栏：页内返回交给状态栏返回键（App 实现 `on_back`），主页交给状态栏主页键
- 全局浮层挂在 `lv_layer_top()`，并确保 `lv_layer_top()` 本身不吞触摸
- 屏幕切换动画统一 `LV_SCREEN_LOAD_ANIM_FADE_IN` + 250 ms；主题重建用 `LV_SCREEN_LOAD_ANIM_NONE`
- 反馈统一 `fw_ui_toast()`（1500 ~ 2500 ms），确认类操作统一 `fw_ui_dialog()`
- LVGL 键盘没有中文 IME：需要中文输入的界面必须给替代入口（如天气 App 的常用城市快捷钮），提示文案写明只能输拼音 / 英文
- 带软键盘的浮层：点浮层空白处收起键盘并让输入框失焦（用 `LV_EVENT_CLICKED` 判 `lv_event_get_target()` 是不是浮层本身，别把子控件的点击也算进去）；同时给一个显式的「重置」入口清空并复位，免得焦点残留在输入框上
- 所有文案遵循项目的术语约定（`Wi-Fi`、`I2C`、`SPIFFS`、数字与单位间加空格等）

### 12.6 App 页面结构（时钟 / 日历 / 天气沉淀的风格）

写新 App 照这一节来，不要再自创一套。参考实现：`app_clock.c`、`app_calendar.c`、`app_weather.c`。

**1. 一屏放下，不滚动**

- 可视高度 = 240 − 状态栏 28 − 上下内边距 24 = **188 px**。先把每块高度加起来算一遍，宁可压行高也不让用户滚动；确实放不下才用可滚动容器，并把页面本身的滚动显式关掉（避免误触）。
- 需要多页时，在同一个根屏里放几块同尺寸容器做显隐，返回键交给 `on_back`（见 `03-app-framework.md` 5.1）。

**2. 纵向骨架（从上到下）**

| 块 | 规格 |
|----|------|
| 头部行（可选） | 30 px：左侧主操作 / 信息（撑满剩余宽度），右侧图标按钮（36 px，`icon_ui_*`）；按钮统一加 `lv_obj_set_ext_click_area(btn, 4)`。**没有状态信息或头部动作的页面不放**（如时钟把设置项集中在主页），不要为了"有个标题"硬加一行 |
| 主信息卡 | 60 ~ 70 px：`fw_ui_hero_card()`（`bg_card` + **2 px `accent` 描边** + 圆角 12 + 内边距 10）；第一行「大数值（24 px）+ 状态（16 px、强调色）」用 flex 行两端对齐、垂直居中，第二行次要小字（14 px、次要色）。它是页面的主角，不要做成和设置行一样的普通卡片 |
| 详情格 | 3 列 × 3 行、格子 96 × 22、间距 4；每格 `bg_card` + 1 px 描边 + 圆角 6，格内**标题靠左（次要色）、数值靠右（主色）**两端对齐 |
| 设置行 | `fw_ui_row_btn*()` / `fw_ui_slider_row()`：高 44、**卡片内的行**（透明底 + 底部 1 px `divider`），不要直接摆在页面上散成一堆下划线；用 `fw_ui_group(parent)` 建一张分组卡片把它们包起来，一页 1~2 张卡、卡内是列表。按功能分组，不同组各一张卡。加完行对本张卡调用一次 `fw_ui_group_end()` 去掉末行分隔线 —— 卡片自身有描边，再留一条就是双线 |
| 行距 | 用 `fw_ui_page` 默认的 8 px；不够时页面容器自己设 6 px（天气页放 24 px 大数值时用 4 px） |

**浮层统一版式**（天气城市搜索、Wi-Fi 密码输入都照这个来）：

| 位置 | 内容 |
|------|------|
| 第一行（高 30） | 提示文字（14 px、次要色，撑满）；需要时再加 `fw_ui_btn()` 动作按钮（50 × 30、圆角 8）：主动作用主色底 + 白字，次要动作用卡片底 + 描边。**带键盘的浮层，主提交动作交给键盘回车（`LV_EVENT_READY`），不再放"连接 / 确定"这类主色按钮** |
| 第二行（y = 36） | 输入框 `fw_ui_textarea()`（高 36、撑满）+ 可选的内联控件（如"显示密码"复选框） |
| 下方 | 浮层自己的内容（结果列表 / 快捷钮），超出部分自己滚动 |
| 底部 | 软键盘，默认隐藏，点输入框才弹出；尺寸 = 屏宽 − 16、高 120，`LV_ALIGN_BOTTOM_MID` 偏移 −8（四周留 8）；收起键盘要 `lv_keyboard_set_textarea(kb, NULL)`，否则输入框的 FOCUSED 不清、再点弹不出来 |
| 交互 | 点浮层空白处收键盘并让输入框失焦；返回键先关浮层；需要清空时给显式「重置」入口 |

**设置 / 列表型页面**：不必凑主信息卡与九宫格，用「头部行 + 内容」两块即可。头部放状态文字（撑满剩余宽度）+ 图标按钮（Wi-Fi、蓝牙都这么做）；内容放 `fw_ui_list()` 或若干 `fw_ui_row_btn()` / `fw_ui_slider_row()`。内容超过一屏时只让列表自身滚动，页面容器显式 `lv_obj_set_scrollable(page, false)`；需要多页时在同一个根屏里放几块同尺寸容器做显隐，返回交给 `on_back`（见 `app_wifi.c`、`app_bt.c`、`app_perf.c`、`app_about.c`）。

**列表项多的时候要分段建**：目录 / 曲库 / 图库动辄几百项，一次循环全建完会让 LVGL 任务（连带触摸、状态栏）停住几百毫秒。用 `fw_ui_stage_create()` + `fw_ui_stage_start()` 每约 16 ms 建一批（文件列表 12 项、图库 6 个格子一批），构建期间界面仍可交互，已经建好的项也能点。要点：`build` 回调按序号建第 idx 项；容器被删之前先 `fw_ui_stage_stop()`；句柄在 `on_destroy` 里 `fw_ui_stage_destroy()`（见 `app_file.c`、`app_image.c`、`app_music.c`、`app_editor.c`）。

**3. 网格必须左对齐**

`lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, ...)`：塞不满一行时（例如 8 项放 9 宫格），`SPACE_BETWEEN` 会把最后一格推到最右边，看着像"跳到第三列"；`START` 让不满的行左对齐、优先充满格位。行满时两者一致。

**4. 没数据也要渲染骨架**

类目先全部建出来、数值用 `--` 占位，主信息卡写「无数据」。状态 / 错误（`请选择城市`、`加载中…`、`未联网`、`请求失败`）放在**固定位置**（主信息卡右上角），不要把整页清空或只在第一格写一句话。

**5. 定时刷新与生命周期**

- 定时刷新用固定周期轮询 + 判断"要显示的值有没有变"（时钟：100 ms 轮询 + 秒变化），不要用 `lv_timer_set_period()` 去对齐相位。
- 定时器在 `on_pause` 暂停，`on_start` 与 `on_resume` **都要**恢复；`on_create` 里创建后先暂停。

**6. 交互与浮层**

- 点击 `LV_EVENT_SHORT_CLICKED`、长按 `LV_EVENT_LONG_PRESSED`、滑块 / 复选框 `LV_EVENT_VALUE_CHANGED`。
- 需要确认的操作走 `fw_ui_dialog()`；反馈走 `fw_ui_toast()`（同一时刻只保留一个）。
- 带软键盘的浮层：点浮层空白处收起键盘并让输入框失焦，同时给一个显式的「重置」入口（见 12.5）。

**7. 颜色与字体**

只用 `fw_theme_color_*()`，控件创建时显式设色（浅色主题下不设色会白字白底看不见）；字号层级固定：时钟主时间 32 px（`fw_asset_font_32()`）、大数值 24 px、标题 16 px（`fw_asset_font_cn_large()`）、正文 14 px（`fw_asset_font_cn()`）。

### 12.7 新增界面自检清单

1. 颜色只从 `fw_theme_color_*()` 取，不依赖 LVGL 自带主题
2. 卡片 / 按钮 / 浮层都有 1 px `border`，`shadow_width = 0`
3. 点击用 `SHORT_CLICKED`，滑块 / 复选框用 `VALUE_CHANGED`
4. 文字走 `fw_asset_font_*`；字体覆盖不到的字（GBK 之外）改 `tools/gen_fw_fonts.py` 的字符集后重新生成
5. 深浅两个主题各看一遍（显示 App → 主题），确认没有看不见的控件、没有糊成一团的层次
6. 跑 `python tools/check_ui_colors.py` 与 `python tools/check_cn_text.py`
