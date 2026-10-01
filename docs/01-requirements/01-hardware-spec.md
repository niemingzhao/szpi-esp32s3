# SZPI-OS 硬件规格

## 总线与接口

### I2C 总线

- SDA：GPIO1
- SCL：GPIO2
- 时钟频率：100 kHz
- 主机：I2C0

#### I2C 总线从设备

| 设备 | I2C 地址（7-bit） | 寄存器宽度 |
|------|----------------|-----------|
| QMI8658 | 0x6A | 8-bit |
| PCA9557PW | 0x19 | 8-bit |
| FT6336 | 0x38 | 8-bit |
| ES8311 | 0x18 | 8-bit |
| ES7210 | 0x41 | 8-bit |
| GC0308（SCCB） | 0x21 | 8-bit |
| GC2145（SCCB） | 0x3C | 8-bit |

- 摄像头 SCCB（GC0308 / GC2145）由独立的 I2C1 控制器驱动，引脚复用 GPIO1 / GPIO2（与 I2C0 共用）

### SPI 总线（LCD 专用）

| 信号 | 引脚 | 说明 |
|------|------|------|
| SCLK | GPIO41 | SPI 时钟 |
| MOSI | GPIO40 | SPI 数据（无 MISO，单向） |
| DC | GPIO39 | 数据/命令选择 |
| CS | PCA9557.BIT0 | 由 IO 扩展芯片控制 |
| RST | NC | 硬件未接，靠 ST7789 软件复位 |
| BL（背光） | GPIO42 | LEDC PWM 控制 |

- 主机：SPI3_HOST
- 时钟频率：80 MHz
- SPI 模式：SPI_MODE2（CPOL=1, CPHA=0）
- 像素格式：RGB565，16-bit
- 显示分辨率：320×240
- 方向控制：已交换 XY + 镜像 X

### I2S 总线（音频）

| 信号 | 引脚 | 说明 |
|------|------|------|
| MCLK | GPIO38 | 主时钟 |
| BCLK | GPIO14 | 位时钟 |
| WS | GPIO13 | 左右声道选择 |
| DOUT | GPIO45 | 数据输出（到 ES8311 DAC） |
| DIN | GPIO12 | 数据输入（从 ES7210 ADC） |

- 主机：I2S0（GPIO 矩阵映射）
- 采样率：8 kHz / 16 kHz / 22.05 kHz / 32 kHz / 44.1 kHz / 48 kHz 可配置（录音侧 ES7210 仅支持 16 kHz / 44.1 kHz / 48 kHz）
- 位宽：16-bit
- 通道数：2（Stereo）
- MCLK：由 I2S 主机输出，固定为采样率 × 256

### 音频器件连接

- ES7210：4 通道，使用 3 路（2 路 MIC + 1 路 ES8311 回环，回声消除）
- ES8311：仅使用输出，分两路（ES7210 回声消除、NS4150B 功放）
- NS4150B：单声道 D 类功放，PA_EN = PCA9557.BIT1，默认关闭

### SDMMC 总线（TF 卡）

| 信号 | 引脚 | 说明 |
|------|------|------|
| CLK | GPIO47 | SDMMC 时钟 |
| CMD | GPIO48 | 命令线 |
| D0 | GPIO21 | 数据线 0 |

- 模式：1-SD（1-bit）
- 内部上拉：启用（SDMMC_SLOT_FLAG_INTERNAL_PULLUP）
- 默认挂载点：/sdcard

### 摄像头接口（DVP）

| 信号 | 引脚 |
|------|------|
| D0 | GPIO16 |
| D1 | GPIO18 |
| D2 | GPIO8 |
| D3 | GPIO17 |
| D4 | GPIO15 |
| D5 | GPIO6 |
| D6 | GPIO4 |
| D7 | GPIO9 |
| XCLK | GPIO5 |
| PCLK | GPIO7 |
| VSYNC | GPIO3 |
| HREF | GPIO46 |
| SIOC（SCCB SCL） | GPIO2（与 I2C0_SCL 复用） |
| SIOD（SCCB SDA） | GPIO1（与 I2C0_SDA 复用） |
| PWDN | PCA9557.BIT2 |
| RESET | NC |

- 传感器：GC0308（VGA 640×480，SCCB 0x21）/ GC2145（UXGA 1600×1200，SCCB 0x3C），由 esp32-camera 按 PID 自动识别
- 像素格式：RGB565；拍照由软件编码为 BMP
- XCLK 频率：24 MHz
- 帧缓冲：2 个，存放于 PSRAM
- 抓取模式：CAMERA_GRAB_WHEN_EMPTY

### USB 接口（原生 USB）

| 信号 | 引脚 |
|------|------|
| USB_DP | GPIO20 |
| USB_DM | GPIO19 |

- 接口：ESP32-S3 原生 USB-OTG，经 USB-HUB（CH334F）下行引出到 Type-C
- 模式：仅 USB Serial/JTAG（默认调试），不启用 OTG 主机/设备模式

### 串口（UART0 / CH340K）

| 信号 | 引脚 | 说明 |
|------|------|------|
| U0TXD | GPIO43 | 接 CH340K |
| U0RXD | GPIO44 | 接 CH340K |

- CH340K 连接到 ESP32-S3 串口 0（UART0）
- 用途：程序下载、串口调试

### USB-HUB 拓扑

- Type-C 接 CH334F（USB 2.0 HUB）上行；4 个下行口使用 2 个
- 下行 D3：ESP32-S3 原生 USB-OTG（GPIO19/20）
- 下行 D4：CH340K → UART0（GPIO43/44）

## 外设引脚与配置

### 触摸（FT6336）

- SDA：GPIO1（与 I2C0 共享）
- SCL：GPIO2（与 I2C0 共享）
- RST：NC
- INT：NC（轮询模式）
- I2C 地址：0x38
- 触摸协议：esp_lcd_touch_ft5x06

坐标设置（与 LCD 匹配）：

- x_max = 240（V_RES，反转后）
- y_max = 320（H_RES，反转后）
- swap_xy = 1, mirror_x = 1, mirror_y = 0

### 姿态传感器（QMI8658）

- SDA：GPIO1（与 I2C0 共享）
- SCL：GPIO2（与 I2C0 共享）
- I2C 地址：0x6A
- 中断：NC（默认轮询，可选接到 GPIO）

默认配置：

- ACC：±4g，250 Hz 输出率
- GYR：±512 dps，250 Hz 输出率

### 用户按键

| 信号 | 引脚 | 说明 |
|------|------|------|
| BOOT 键 | GPIO0 | 任意边沿中断，内部上拉；正常运行时为用户按键 |
| RESET 键 | EN | 系统复位 |

## Strapping 引脚

GPIO0、GPIO3、GPIO45、GPIO46 为 strapping 引脚。

| 引脚 | 板上功能 | 说明 |
|------|----------|------|
| GPIO0 | BOOT 键 | 与 GPIO46 决定启动模式，低电平进入下载 |
| GPIO3 | 摄像头 VSYNC | 复位时选择 JTAG 信号源 |
| GPIO45 | I2S_DOUT | 复位时选择 VDD_SPI 电压 |
| GPIO46 | 摄像头 HREF | 与 GPIO0 决定启动模式；下载时必须为低，板上有下拉 |

- GPIO3/45/46 同时复用为摄像头与音频信号，上电时为高阻或低电平

## IO 扩展芯片（PCA9557PW）

| PCA9557 输出位 | 功能 | 默认值 |
|---------------|------|--------|
| BIT0 | LCD_CS | 1（高，SPI 设备未选中） |
| BIT1 | PA_EN（音频功放使能） | 0（关闭） |
| BIT2 | DVP_PWDN（摄像头掉电） | 1（掉电） |

寄存器：

- 0x00：INPUT_PORT
- 0x01：OUTPUT_PORT
- 0x02：POLARITY_INVERSION_PORT
- 0x03：CONFIGURATION_PORT（1=输入，0=输出）

默认配置：BIT0/BIT1/BIT2 设为输出，其他保持输入 → CONFIGURATION_PORT = 0xF8

- 使用 LCD_CS / PA_EN / DVP_PWDN 前，需先初始化 PCA9557

## 引脚汇总（按 GPIO 编号排序）

| GPIO | 功能 |
|------|------|
| 0 | BOOT 按键 |
| 1 | I2C0_SDA |
| 2 | I2C0_SCL |
| 3 | VSYNC（摄像头） |
| 4 | D6（摄像头） |
| 5 | XCLK（摄像头） |
| 6 | D5（摄像头） |
| 7 | PCLK（摄像头） |
| 8 | D2（摄像头） |
| 9 | D7（摄像头） |
| 10 | 外扩接口（GH1.25 多功能） |
| 11 | 外扩接口（GH1.25 多功能） |
| 12 | I2S_DIN |
| 13 | I2S_WS |
| 14 | I2S_BCLK |
| 15 | D4（摄像头） |
| 16 | D0（摄像头） |
| 17 | D3（摄像头） |
| 18 | D1（摄像头） |
| 19 | USB_DM |
| 20 | USB_DP |
| 21 | SD_D0 |
| 22~25 | 未用 |
| 26~32 | 模组内 Flash/PSRAM（未引出） |
| 33~37 | 八线 PSRAM 占用（不可用） |
| 38 | I2S_MCLK |
| 39 | LCD_DC |
| 40 | LCD_MOSI |
| 41 | LCD_SCLK |
| 42 | LCD_BL（背光 PWM） |
| 43 | UART0_TX（接 CH340K） |
| 44 | UART0_RX（接 CH340K） |
| 45 | I2S_DOUT |
| 46 | HREF（摄像头） |
| 47 | SD_CLK |
| 48 | SD_CMD |

## 外扩接口（GH1.25）

- 接口数：2 路
- 端子规格：GH1.25 5P
- 供电：3.3V 和 5V（可输出给外部传感器）
- 接口类型：
  - 第 1 路：I2C 接口（与板上 I2C 总线共用）
  - 第 2 路：多功能接口
- 多功能接口信号：GPIO10、GPIO11
- 多功能接口复用：GPIO / UART / CAN / PWM
- 电压注意：ESP32-S3 GPIO 为 3.3V，5V 信号需要电平转换

## 电源

- SY8088AAC 双路输出，每路 1A：MCU 电路 3V3，音频电路 AU_3V3
- 供电：Type-C
