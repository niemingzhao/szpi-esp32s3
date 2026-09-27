# SZPI-OS 原始需求

## 项目名称

SZPI-OS

## 产品定位

运行在立创·实战派 ESP32-S3 开发板上的嵌入式操作系统固件。

## 硬件清单

- 模组：ESP32-S3-WROOM-1-N16R8（Xtensa LX7 双核 240 MHz / 内置 SRAM 512 KB / 外置 PSRAM 8 MB Octal 80 MHz / 外置 Flash 16 MB / Wi-Fi 802.11 b/g/n 2.4 GHz 40 MHz 带宽 / Bluetooth 5 LE + Bluetooth Mesh / 集成 AI 向量指令）
- 显示屏：ST7789，2.0 寸 IPS 全视角，分辨率 320×240，SPI 接口
- 触摸屏：FT6336，电容触摸，I2C 接口
- 姿态传感器：QMI8658，三轴加速度 + 三轴陀螺仪，I2C 接口
- 音频 DAC：ES8311，单通道，I2C 接口
- 音频 ADC：ES7210，四通道（板子使用三通道），I2C 接口
- 音频功放：NS4150B，单声道 D 类音频放大器
- 麦克风：ZTS6216，双路模拟输出
- 喇叭：DB1811AB50，1811 音腔喇叭，1 W
- USB HUB：CH334F，USB 2.0 HUB（板子用 4 口下行中的 2 口）
- USB 转串口：CH340K，波特率最大 2 Mbps
- 电源芯片：SY8088AAC，提供双路，每路 1 A（MCU 电路与音频电路独立供电）
- 摄像头：兼容 GC0308（30 万像素）与 GC2145（200 万像素），BTB 连接器，DVP 接口
- TF 卡接口：1-SD 模式，SDMMC 连接 ESP32
- 按键：1 个复位按键 + 1 个用户自定义按键（BOOT，GPIO0）
- Type-C 接口：供电、程序下载、程序调试、USB 数据通信
- IO 扩展芯片：PCA9557PW，I2C 接口，扩展 3 路 IO（LCD_CS、PA_EN、DVP_PWDN）
- 外扩接口：1 路 I2C 接口 + 1 路多功能接口（GH1.25 5P 端子，可输出 3.3 V 和 5 V，引出 GPIO10、GPIO11，支持 GPIO / UART / CAN / PWM）
- 板子尺寸：69 × 41 × 14 mm

## 核心功能范围

### 系统与内核

- 启动序列
- 多任务调度（FreeRTOS）
- 看门狗
- 异常处理与 crash log
- 系统信息查询（CPU / 内存 / Flash）
- 软复位 / 硬复位
- 电源管理（关机 / 重启）
- 时间管理（系统时钟 / 时区 / 12 / 24 小时制）
- 屏幕超时熄屏 / 唤醒
- 亮度调节
- 音量调节

### 脚本系统

- 脚本运行时（Lua 5.5）
- 脚本列表与元信息（名称 / 说明 / 入口）
- 脚本生命周期管理（启动 / 停止 / 异常退出）
- 脚本错误处理与日志
- 脚本配置持久化
- 脚本权限与沙箱（限制可访问的系统能力）
- 内置示例脚本
- 网络脚本下载

### 脚本能力

- 显示与界面（LVGL 控件 / 页面 / 图片 / 画布）
- 输入（触摸 / IMU 姿态 / 物理按键）
- 音频播放 / 录音
- 摄像头拍照
- GPIO / PWM / I2C / UART / ADC / CAN
- 文件读写（TF 卡 / 内置 Flash）
- 网络（HTTP / HTTPS / MQTT / WebSocket）
- 蓝牙 BLE
- 时间与系统信息
- 事件订阅与发布
- 系统通知（Toast）
- 网页服务（局域网）

### UI 系统

- 主题系统（深色 / 浅色）
- 主题运行时切换
- 字体系统（多字号 / 中英文）
- 图片资源系统
- 图标系统
- 通用 UI 组件（进度条 / 对话框 / Toast / 列表 / 网格）
- 状态栏（返回按钮、主页按钮、时间、Wi-Fi、蓝牙 BLE、声音、录音、摄像头、TF 卡）
- 桌面（图标网格）

### 输入系统

- 触摸输入
- IMU 姿态输入
- 物理按键输入
- 输入防抖

### 多媒体

- 音频播放（MP3）
- 录音与播放（WAV）
- 摄像头预览
- 摄像头拍照（BMP）
- 图片查看（PNG / JPEG / GIF / BMP）

### 通信

- Wi-Fi 配网与管理
- 蓝牙 BLE 连接与管理（从机 / 中心角色）
- 时间同步（SNTP）
- 时区设置
- HTTP 客户端
- HTTPS 支持
- MQTT 客户端
- WebSocket 客户端
- 蓝牙 HID 设备模拟
- 局域网 Web 管理页

### 存储

- TF 卡挂载（FAT）
- TF 卡热插拔
- 文件读写
- 内置 Flash 文件系统（SPIFFS）
- 文件系统格式化
- 存储空间查询
- NVS 配置存储

### 传感器

- 6 轴姿态读取（加速度 / 陀螺仪）
- 姿态变化检测
- 摇晃检测
- 抬手检测

### 内置应用

- 脚本管理
- 时钟
- 日历
- 天气
- Wi-Fi
- 蓝牙 BLE
- 显示
- 声音
- 文件管理
- 文本编辑
- 计算器
- 下载器
- 音乐
- 录音机
- 相机
- 图库
- 秒表
- 计时器
- 姿态仪
- 性能监控（CPU / 内存 / Flash / TF 卡）
- 系统日志（分级 / TAG）
- 关于本机

## 交互方式

- 触摸：点击、长按、桌面左右滑动翻页
- IMU 姿态：姿态变化、摇晃、抬手
- 物理按键：单击、双击、长按

## 开发环境

- ESP-IDF 版本：v6.1
- ESP-IDF 安装路径：C:\esp\v6.1\esp-idf
- 工具链路径：C:\Espressif\tools
- 操作系统：Windows
- IDE：VSCode + ESP-IDF 插件

## 开源协议

MIT License

## 文档范围

- 01-requirements：需求文档
- 02-architecture：架构设计
- 03-design：详细设计

## 补充资料清单

| 类别 | 来源 | 路径 | 用途 |
|------|------|------|------|
| 官方 Wiki | 立创开发板 | https://wiki.lckfb.com/zh-hans/szpi-esp32s3/ | 板子简介、外设清单 |
| 官方 Wiki | 立创开发板 | https://wiki.lckfb.com/zh-hans/szpi-esp32s3/open-source-hardware/ | 原理图、3D 外壳下载入口 |
| 参考项目 | 虾哥 | https://github.com/78/xiaozhi-esp32 | 小智 AI 语音对话设备（与本板子兼容） |
