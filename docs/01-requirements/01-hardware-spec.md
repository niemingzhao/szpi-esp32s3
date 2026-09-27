# 硬件规格

## 总线与接口

### I2C 总线

- SDA：GPIO1
- SCL：GPIO2
- 时钟频率：100 kHz（默认）/ 400 kHz（FT6336 用）
- 主机：I2C0

#### I2C 总线从设备

| 设备 | I2C 地址 (7-bit) | 寄存器宽度 |
|------|----------------|-----------|
| QMI8658 | 0x6A | 8-bit |
| PCA9557PW | 0x19 | 8-bit |
| FT6336 | 0x38 | 8-bit |
| ES8311 | 0x18 | 8-bit |
| ES7210 | 0x41 | 8-bit |
| GC0308 (SCCB) | 0x21 | 8-bit |

### SPI 总线（LCD 专用）

| 信号 | 引脚 | 说明 |
|------|------|------|
| SCLK | GPIO41 | SPI 时钟 |
| MOSI | GPIO40 | SPI 数据（无 MISO，单向） |
| DC | GPIO39 | 数据/命令选择 |
| CS | PCA9557.BIT0 | 由 IO 扩展芯片控制 |
| RST | NC | 硬件未接，靠 ST7789 软件复位 |
| BL (背光) | GPIO42 | LEDC PWM 控制 |

- 主机：SPI3_HOST
- 时钟频率：80 MHz
- SPI 模式：SPI_MODE2（CPOL=1, CPHA=1）
- 像素格式：RGB565, 16-bit
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
| PA_EN | PCA9557.BIT1 | 功放使能，由 IO 扩展芯片控制 |

- 主机：I2S1 (I2S_NUM_1)
- 采样率：16 kHz / 48 kHz 可配置
- 位宽：16-bit
- 通道数：2 (Stereo)
- MCLK 倍频：384（MCLK = 16k × 384 = 6.144 MHz）

### SDMMC 总线（TF 卡）

| 信号 | 引脚 | 说明 |
|------|------|------|
| CLK | GPIO47 | SDMMC 时钟 |
| CMD | GPIO48 | 命令线 |
| D0 | GPIO21 | 数据线 0 |

- 模式：1-SD (1-bit)
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
| SIOC (SCCB SCL) | GPIO2（与 I2C0_SCL 复用） |
| SIOD (SCCB SDA) | GPIO1（与 I2C0_SDA 复用） |
| PWDN | PCA9557.BIT2 |
| RESET | NC |

- 像素格式：JPEG / RGB565
- XCLK 频率：24 MHz
- 帧缓冲：2 个，存放于 PSRAM
- 抓取模式：CAMERA_GRAB_WHEN_EMPTY

### USB 接口

| 信号 | 引脚 |
|------|------|
| USB_DP | GPIO20 |
| USB_DM | GPIO19 |

- 模式：仅 USB Serial/JTAG（默认调试）
- 不启用 USB OTG 主机/设备模式
- 用途：供电、CH340K 串口下载、调试

## 外设引脚与配置

### 触摸 (FT6336)

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

### 姿态传感器 (QMI8658)

- SDA：GPIO1（与 I2C0 共享）
- SCL：GPIO2（与 I2C0 共享）
- I2C 地址：0x6A
- 中断：NC（默认轮询，可选接到 GPIO）

默认配置：

- ACC：±4g, 250Hz 输出率
- GYR：±512 dps, 250Hz 输出率

### 用户按键

| 信号 | 引脚 | 说明 |
|------|------|------|
| BOOT 键 | GPIO0 | 下降沿中断 |

## IO 扩展芯片 (PCA9557PW)

| PCA9557 输出位 | 功能 | 默认值 |
|---------------|------|--------|
| BIT0 | LCD_CS | 1（高，SPI 设备未选中） |
| BIT1 | PA_EN（音频功放使能） | 0（关闭） |
| BIT2 | DVP_PWDN（摄像头掉电） | 1（掉电） |

寄存器：

- 0x00：INPUT_PORT
- 0x01：OUTPUT_PORT
- 0x02：POLARITY_INVERSION_PORT
- 0x03：CONFIGURATION_PORT（1=输入, 0=输出）

默认配置：BIT0/BIT1/BIT2 设为输出，其他保持输入 → CONFIGURATION_PORT = 0xF8

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
| 10 | 未用 |
| 11 | 未用 |
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
| 22~37 | 未用 |
| 38 | I2S_MCLK |
| 39 | LCD_DC |
| 40 | LCD_MOSI |
| 41 | LCD_SCLK |
| 42 | LCD_BL（背光 PWM） |
| 43~44 | 未用 |
| 45 | I2S_DOUT |
| 46 | HREF（摄像头） |
| 47 | SD_CLK |
| 48 | SD_CMD |

## 外扩接口 (GH1.25)

- 接口数：2 路
- 端子规格：GH1.25 5P
- 供电：3.3V 和 5V（可输出给外部传感器）
- 接口类型：
  - 第 1 路：I²C 接口（与板上 I²C 总线共用）
  - 第 2 路：多功能接口
- 多功能接口信号：GPIO10、GPIO11
- 多功能接口复用：GPIO / UART / CAN / PWM
- 电压注意：ESP32-S3 GPIO 为 3.3V，5V 信号需要电平转换

## 关键 ESP-IDF 配置项 (sdkconfig)

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_BT_ENABLED=y
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_RX_BA_WIN=6
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096
CONFIG_FATFS_CODEPAGE_936=y
CONFIG_FATFS_API_ENCODING_UTF_8=y
CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4
CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE=y
CONFIG_SPIFFS_OBJ_NAME_LEN=128
CONFIG_LV_COLOR_16_SWAP=y
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_32=y
CONFIG_LV_FONT_FMT_TXT_LARGE=y
CONFIG_LV_USE_PNG=y
CONFIG_LV_USE_GIF=y
```

## 分区表

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  24k
phy_init, data, phy,     0xf000,  4k
factory,  app,  factory, ,        4M
ota_0,    app,  ota_0,   ,        4M
ota_1,    app,  ota_1,   ,        4M
storage,  data, spiffs,  ,        3M
```

合计：bootloader (32KB @ 0x0000) + partition table (12KB @ 0x8000) + 6 个分区 (24K + 4K + 4M + 4M + 4M + 3M ≈ 15M) ≈ 15.07MB，16MB Flash 余量约 1MB