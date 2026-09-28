# SZPI-OS 原始需求

## 项目名称

SZPI-OS

## 产品定位

运行在立创·实战派 ESP32-S3 开发板上的嵌入式操作系统固件。

## 硬件清单

- 模组：ESP32-S3-WROOM-1-N16R8（Xtensa LX7 双核 240MHz / 内置 SRAM 512KB / 外置 PSRAM 8MB Octal 80MHz / 外置 Flash 16MB / Wi-Fi 802.11 b/g/n 2.4GHz 40MHz 带宽 / Bluetooth 5 LE + Bluetooth Mesh / 集成 AI 向量指令）
- 显示屏：ST7789，2.0 寸 IPS 全视角，分辨率 320×240，SPI 接口
- 触摸屏：FT6336，电容触摸，I2C 接口
- 姿态传感器：QMI8658，三轴加速度 + 三轴陀螺仪，I2C 接口
- 音频 DAC：ES8311，单通道，I2C 接口
- 音频 ADC：ES7210，四通道（板子使用三通道），I2C 接口
- 音频功放：NS4150B，单声道 D 类音频放大器
- 麦克风：ZTS6216，双路模拟输出
- 喇叭：DB1811AB50，1811 音腔喇叭，1W
- USB HUB：CH334F，USB 2.0 HUB（板子用 4 口下行中的 2 口）
- USB 转串口：CH340K，波特率最大 2 Mbps
- 电源芯片：SY8088AAC，提供双路，每路 1A（MCU 电路与音频电路独立供电）
- 摄像头：GC0308，30 万像素，BTB 连接器，DVP 接口，单电源 2.8V
- TF 卡接口：1-SD 模式，SDMMC 连接 ESP32
- 按键：1 个复位按键 + 1 个用户自定义按键（BOOT，GPIO0）
- Type-C 接口：供电、程序下载、程序调试、USB 数据通信
- IO 扩展芯片：PCA9557PW，I2C 接口，扩展 3 路 IO（LCD_CS、PA_EN、DVP_PWDN）
- 外扩接口：1 路 I2C 接口 + 1 路多功能接口（GH1.25 5P 端子，可输出 3.3V 和 5V，引出 GPIO10、GPIO11，支持 GPIO/UART/CAN/PWM）
- 板子尺寸：69 × 41 × 14 mm

## 核心功能范围

### 系统与内核

- 启动序列
- 多任务调度（FreeRTOS）
- 双核管理
- 看门狗
- 异常处理与 crash log
- 系统信息查询（内存 / Flash / CPU 占用）
- 时间管理（系统时钟 / RTC / 时区）
- 软复位 / 硬复位
- 电源管理（休眠 / 唤醒 / 续航模式）
- OTA 在线升级

### UI 系统

- 主题系统（深色 / 浅色）
- 主题运行时切换
- 字体系统（多字号 / 中英文）
- 图片资源系统
- 图标系统
- 通用 UI 组件（进度条 / 对话框 / Toast）
- 状态栏
- 应用列表（应用抽屉）
- 应用切换
- 桌面（图标网格）
- 虚拟按键栏
- 过渡动画
- 音量调节
- 屏幕亮度调节
- 屏幕超时熄屏
- 屏幕唤醒
- 屏幕旋转（0/90/180/270）
- 多语言支持（i18n）

### 输入系统

- 触摸输入
- 物理按键输入
- 手势识别
- IMU 姿态输入
- 多击识别（单击 / 双击 / 三击）
- 长按识别
- 按键映射配置
- 输入防抖

### 通知系统

- 通知发送
- 通知接收
- 通知中心
- 通知分类
- 通知持久化
- 通知优先级
- 通知点击回调
- 通知提示音
- 免打扰模式
- 应用角标

### 多媒体

- MP3 播放
- WAV 录音与播放
- 音频播放控制（音量 / 暂停 / 恢复 / 切换）
- 麦克风采集
- 扬声器控制
- 摄像头预览
- 图像采集（拍照保存）
- 图片查看
- 视频播放（轻量）

### 通信

- Wi-Fi 扫描
- Wi-Fi 连接（STA）
- Wi-Fi 自动重连
- Web 配网
- SmartConfig 配网
- 蓝牙 HID 设备模拟
- 蓝牙扫描
- 蓝牙 BLE 外设模式
- 时间同步（SNTP）
- 时区设置
- HTTP 客户端
- HTTPS 支持
- MQTT 客户端
- WebSocket 客户端

### 存储

- TF 卡挂载（FAT）
- SD 卡热插拔
- 文件读写
- 内置 Flash 文件系统（SPIFFS）
- 文件系统格式化
- 磁盘空间查询
- NVS 配置存储
- NVS 加密
- 路径管理
- 应用数据隔离
- 文件浏览器

### 传感器

- 6 轴姿态读取
- 姿态变化检测
- 运动状态检测（Any-Motion / No-Motion / Significant-Motion）
- 屏幕自动旋转
- 抬手检测
- 摇晃检测
- 敲击检测
- 计步器

### 应用框架

- 应用注册表
- 应用生命周期管理
- 应用消息总线
- 应用间通信
- 应用配置隔离
- 应用设置持久化
- 应用启动参数
- 应用图标配置
- 应用退出机制
- 应用权限管理
- 应用沙箱

### 调试与诊断

- 系统日志（分级 / TAG）
- 串口命令行（shell）
- 性能监控（CPU / 内存 / 任务）
- 调试控制台

### 内置应用

- 时钟
- 设置
- 音乐播放器
- 录音机
- 图片查看器
- 视频播放器
- 相机
- 文件管理器
- 文本编辑器
- 计算器
- 姿态传感器显示
- 蓝牙 HID 模拟器
- 浏览器
- OTA 升级
- 调试控制台
- 关于本机
- 工厂模式

## 交互方式

- 触摸：点击、滑动、长按、多点
- IMU 手势：姿态变化、摇晃、敲击、抬手
- 物理按键：单击、双击、三击、长按

## 开发环境

- ESP-IDF 版本：v5.4.4
- ESP-IDF 安装路径：C:\esp\v5.4.4\esp-idf
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
| 参考项目 | cc1234github | https://github.com/cc1234github/lc_shizhanpai_zephyr | 实战派 Zephyr 移植 |
| 参考项目 | GBall5599 | https://github.com/GBall5599/-ESP-CLAW | ESP-CLAW 项目 |
| 参考项目 | umeiko | https://github.com/umeiko/jlc-shizhanpai-esp32s3-arduino-lvgl | 实战派 Arduino + LVGL |
| 本地参考例程 | 立创官方 | D:\workspace\esp32s3\ | 立创官方 14 个实战派 ESP32-S3 例程 |