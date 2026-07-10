# 喵哎-MiaooAim 实现文档

## 实现状态快照（2026-06-28）

当前实现已经覆盖图片转换、图库、轮播、天气、时钟、日历、课程表、待办、倒计时、Codex 额度看板、留言板、画布、OTA、低功耗和外置 fontfs 字库。稳定性基线包括：`display_policy_begin_manual_display()` 统一手动显示 epoch，主要显示入口在最终刷屏前校验 epoch；`/calendar_show`、`/canvas_show`、`/msg_show` 等异步渲染入口等待任务完成后再返回，并在被新请求抢占时返回 `canceled=true`；API/配置/媒体 GET 接口补齐 Basic Auth；`/panel_config` 只保存配置并提示重启；`format_if_mount_failed=false` 保护 SPIFFS 用户内容；挂载失败时完整启动保留 AP/Web，若 EPD 可用会显示 `文件系统恢复` 恢复页；配置备份/恢复排除 Basic Auth；图库上传转换失败会删除坏图；正常启动时 EPD 初始化失败保留 AP/Web；AP 密码不再进入串口日志；低功耗正常启动与 RTC 定时器 quick-refresh 已分离。

v2.3.4 同步了屏幕参考驱动选项和低功耗闲置计时修复：网页后台状态轮询和自动刷屏不再持续刷新用户活跃时间，用户真正停留不操作时设备可按配置进入深睡。

后续实现重点：OTA 与低功耗长稳测试、更多屏幕型号适配，以及在源码、分区表或字库资源变化后保持 Release 包与校验信息同步。

> 项目代号：**epaper_uploader**
> 硬件平台：ESP32-S3 + SSD1619(4.2" 400×300) / UC8179(5.83" 648×480) 三色墨水屏
> 技术栈：ESP-IDF（**最低 v5.5.1**）/ C / SPI DMA / WiFi SoftAP / HTTP Server / TJpgDec / LodePNG
> 固件版本：v2.3.4（`PROJECT_VER`） | 文档版本：v2.3.4-docs | 2026-06-28

---

## 一、项目概述

本项目是一个基于 **ESP32-S3** 的三色电子墨水屏终端固件。用户可通过手机或电脑访问设备 Web 管理端，完成配网、图片上传、画廊轮播、天气、时钟、日历、课程表、待办、倒计时、留言板、画板和额度看板配置；固件侧负责图片解码、三色量化、页面渲染、SPI DMA 传输和低功耗唤醒策略。

**核心数据流：**

```
浏览器(Canvas预览→面板尺寸 PNG) → WiFi SoftAP → HTTP Server(流式接收)
    → SPIFFS文件存储 → LodePNG/TJpgDec 解码
    → 灰度+红色分离 → Floyd-Steinberg抖动 → 1bpp MSB-first打包
    → 黑色/彩色平面打包 → SPI DMA批量传输 → 当前面板驱动 → 墨水屏刷新
```

**系统特性：**

- 浏览器端 Canvas 预览和面板尺寸 PNG 预处理，减少固件侧重复抖动
- JPEG 流式解码（从文件读取，非全量载入内存），降低无 PSRAM 机型的峰值内存压力；带 PSRAM 机型可承载更大的图片和 framebuffer 分配
- Floyd-Steinberg 误差扩散抖动，灰度照片显示效果自然
- SPI DMA 4KB 分块批量传输，数据传输高效
- 画廊管理：存储、预览、选择显示、单张删除
- SPIFFS 文件系统健康检查与数据保护（挂载失败不自动格式化用户内容，完整启动降级保留 AP/Web）

---

## 二、硬件配置

### 2.1 硬件清单

| 组件 | 型号 | 说明 |
|------|------|------|
| 主控 | **ESP32-S3，16MB Flash** | 建议 N16 或 N16R8；N16R8 的 8MB PSRAM 对 5.83" 图片转换和 Web 并发更稳，固件配置允许 PSRAM 缺失时降级运行 |
| 主测墨水屏 | **4.2" SSD1619 BWR / 5.83" UC8179 BWR** | 4.2" 为 400×300，5.83" 为 648×480；两者默认采用稳定全刷 |
| 实验面板 | **ATC/Solum 与 EPD-nRF5 参考面板** | 作为驱动移植入口保留，未标注主测的面板需实机验证清屏、纯黑、纯红、边框和棋盘格 |
| Flash | 内置 16MB | factory + 双 OTA 各 3MB，coredump 128KB，fontfs 2.875MB，用户 SPIFFS 3.875MB |

### 2.2 引脚接线表

| 墨水屏引脚 | ESP32-S3 GPIO | 方向 | 说明 |
|-----------|---------------|------|------|
| **SCL (SCLK)** | GPIO 4 | 输出 | SPI 时钟 |
| **SDI (MOSI)** | GPIO 5 | 输出 | SPI 数据输入 |
| **RES** | GPIO 6 | 输出 | 硬件复位，低电平有效 |
| **D/C** | GPIO 7 | 输出 | 数据/命令选择：0=命令，1=数据 |
| **CS** | GPIO 15 | 输出 | SPI 片选，低电平有效 |
| **BUSY** | GPIO 16 | 输入（上拉） | 屏幕忙碌状态，高电平=忙 |
| **VCC** | 3.3V | 电源 | 3.3V 供电 |
| **GND** | GND | 电源 | 公共地 |

> **注意**：MISO 未连接（墨水屏 SPI 为单向通信，只写不读），BUSY 引脚通过 GPIO 轮询判断屏幕状态。

### 2.3 SPI 配置

| 参数 | 值 | 说明 |
|------|------|------|
| SPI 主机 | `SPI2_HOST` | ESP32-S3 的第二个 SPI 外设 |
| 时钟频率 | **2 MHz** | SSD1619 规格书推荐的安全频率 |
| SPI 模式 | **Mode 0** | CPOL=0, CPHA=0（空闲低电平，上升沿采样） |
| DMA 通道 | `SPI_DMA_CH_AUTO` | 自动分配 DMA 通道 |
| 最大传输长度 | 16384 字节 | `max_transfer_sz` 配置 |
| 片选 | 硬件管理 | `spics_io_num = GPIO15`，SPI 驱动自动控制 |
| 队列深度 | 1 | 同步传输，无需深队列 |

---

## 三、软件架构

### 3.1 文件结构

```
msp/
├── CMakeLists.txt              # 顶层 CMake
├── partitions.csv              # Flash 分区表（3MB app 三槽 + coredump + fontfs + 用户 SPIFFS）
├── sdkconfig.defaults          # 项目默认配置
├── .gitignore
├── main/
│   ├── CMakeLists.txt          # 组件注册、源文件列表、EMBED_FILES
│   ├── idf_component.yml       # ESP-IDF 组件依赖与版本约束
│   ├── app_main.c              # 入口：双启动路径（完整启动 / 深睡快速刷新）
│   ├── epd_stub.c / epd.h     # EPD 驱动（SSD1619 / UC8179 / ATC-Solum / EPD-nRF5 参考面板，SPI DMA）
│   ├── image_convert.c/h      # 图片解码（JPEG/PNG/BMP → Floyd-Steinberg 抖色）
│   ├── http_app.c/h           # HTTP 服务器（73 条路由 + Basic Auth + OTA）
│   ├── http_gallery.c         # 画廊 HTTP 路由
│   ├── http_features.c        # 功能模块 HTTP 路由
│   ├── http_canvas.c          # 画布留言板 HTTP 路由与素材管理
│   ├── http_internal.h        # HTTP 子系统内部共享声明
│   ├── wifi_manager.c/h       # WiFi AP/STA + 配网 + 自动重连
│   ├── scheduler.c/h          # 画廊轮播调度器
│   ├── weather.c/h            # 和风天气 HTTPS
│   ├── weather_icons_qw.c/h   # 和风天气离线 1-bit 图标库
│   ├── clock_display.c/h      # 时钟表盘
│   ├── calendar_display.c/h   # 万年历 + 农历
│   ├── lunar.c/h              # 农历算法
│   ├── timetable.c/h          # 课程表（7×12 网格 + 学期周次）
│   ├── todo.c/h               # 待办事项（NVS 持久化 + 进度条）
│   ├── countdown.c/h          # 倒计时
│   ├── codex_quota.c/h        # Codex / 中转站额度看板
│   ├── message_board.c/h      # 留言板
│   ├── canvas_board.c/h       # WYSIWYG 画布留言板（JSON→fb_t 渲染）
│   ├── canvas_icons.h         # 内置 1-bit 画布图标库
│   ├── button.c/h             # 三键驱动
│   ├── battery_mon.c/h        # 电池电压检测与百分比估算
│   ├── power_mgr.c/h          # 深度睡眠与电源管理
│   ├── fb_render.c/h          # 帧缓冲渲染引擎
│   ├── font_data.h            # 点阵字库（ASCII + 中文）
│   ├── ui_theme.c/h           # 墨水屏页面通用 UI 主题组件
│   ├── display_policy.c/h     # 显示策略协调
│   ├── display_mode.c/h       # 显示模式注册与切换
│   ├── device_identity.c/h    # 设备标识
│   ├── time_sync.c/h          # SNTP 时间同步
│   ├── nvs_utils.c/h          # NVS 工具函数
│   ├── spiffs_mount.c/h       # SPIFFS 挂载与健康检查（失败不自动格式化）
│   └── lodepng.c/h            # LodePNG 第三方 PNG 解码库
├── web/
│   ├── index.html             # 主页：上传 / 画廊 / 状态
│   ├── config.html            # 配置：WiFi / 轮播 / 天气 / OTA / 低功耗
│   ├── timetable.html         # 课程表编辑
│   ├── todo.html              # 待办事项管理
│   ├── countdown.html         # 倒计时配置
│   └── board.html             # 画布留言板可视化编辑器
├── README.md
├── IMPLEMENTATION.md           # 本文档
└── ROADMAP.md                  # 当前维护路线图
```

### 3.2 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| **应用入口** | `app_main.c` | 双启动路径：完整启动（WiFi+HTTP+全功能）/ 深睡快速刷新 |
| **EPD 驱动** | `epd_stub.c` / `epd.h` | SSD1619 + UC8179 双屏支持，SPI DMA，BWR 三色 |
| **图片转换** | `image_convert.c/h` | JPEG/PNG/BMP 解码、拉伸全屏、Floyd-Steinberg 抖色 |
| **HTTP 服务器** | `http_app.c/h` + `http_gallery.c` + `http_features.c` + `http_canvas.c` | 73 条路由 + Basic Auth + OTA |
| **WiFi 管理** | `wifi_manager.c/h` | AP+STA 共存、NVS 凭据、自动重连 |
| **轮播调度** | `scheduler.c/h` | 画廊顺序/随机自动轮播 |
| **天气** | `weather.c/h` | 和风天气 HTTPS，实时+3 天预报 |
| **天气图标** | `weather_icons_qw.c/h` | 和风天气离线 1-bit 图标库 |
| **时钟** | `clock_display.c/h` | 数字时钟，自动刷新 |
| **万年历** | `calendar_display.c/h` + `lunar.c/h` | 月视图 + 农历/节气/节日 |
| **课程表** | `timetable.c/h` | 7×12 网格 + 学期周次 + 单日/全周视图 |
| **待办事项** | `todo.c/h` | NVS 持久化，进度条+百分比 |
| **留言板** | `message_board.c/h` | 自定义文字/字号/对齐/颜色 |
| **画布留言板** | `canvas_board.c/h` + `canvas_icons.h` | WYSIWYG Web 编辑器→JSON→帧缓冲渲染（文本/形状/图标/图片） |
| **按键驱动** | `button.c/h` | 3 键轮询+去抖+模式切换 |
| **电池检测** | `battery_mon.c/h` | 电池电压采样与百分比估算 |
| **电源管理** | `power_mgr.c/h` | 深度睡眠、闲置检测、唤醒、模式记忆 |
| **帧缓冲渲染** | `fb_render.c/h` | 像素/线/矩形/UTF-8 文字 |
| **UI 主题** | `ui_theme.c/h` | 墨水屏页面框架、标题栏、卡片和页脚 |
| **显示策略** | `display_policy.c/h` | EPD 互斥 + 手动屏标记 |
| **时间同步** | `time_sync.c/h` | SNTP（NTP 阿里云 + pool.ntp.org）|
| **SPIFFS 存储** | `spiffs_mount.c/h` | 挂载/健康检查/画廊目录，挂载失败不自动格式化用户数据 |
| **设备标识** | `device_identity.c/h` | MAC → AP SSID / mDNS |
| **PNG 解码** | `lodepng.c/h` | 第三方纯 C 库（编译时裁剪）|
| **Web 前端** | `web/*.html` | `index` / `config` / `timetable` / `todo` / `countdown` + 三套主题 |

### 3.2.1 显示请求仲裁与异步语义

当前版本把“谁有资格最终刷屏”收敛到 `display_policy`：用户主动触发的显示入口先调用 `display_policy_begin_manual_display()` 获取 epoch，并在最终 `epd_display_*()` 前调用 `display_policy_epoch_is_current(epoch)`。如果用户又上传图片、点画布、按键切换或触发其它显示，新请求会 bump epoch，旧任务在刷屏前退出，避免旧画面反盖新画面。

日历、画布、留言板仍使用内部渲染任务以免阻塞 HTTP 服务器过久，但 HTTP 层已经不再把“任务创建成功”当作“屏幕显示成功”：`/calendar_show`、`/canvas_show`、`/msg_show` 会等待对应渲染任务结束，被新请求抢占时返回 `canceled=true`。后续若做统一 display queue，应保持同样的 epoch 和取消语义。
### 3.3 数据流详解

```
 ┌───────────────────────────────┐
 │         用户手机浏览器         │
 │  选择图片 → 按面板尺寸预览      │
 │  → Floyd-Steinberg 预处理       │
 │  → toBlob('image/png')          │
 └──────────────┬────────────────┘
                │ POST /upload (WiFi SoftAP, 192.168.4.1)
                ▼
 ┌──────────────────────────────────────┐
 │          HTTP Server (http_app.c)    │
 │  1. 流式接收 → /spiffs/upload.jpg   │
 │  2. 复制到 /spiffs/images/img_xxx   │
 │  3. 调用 image_convert_file_to_epd_raw() │
 └──────────────┬───────────────────────┘
                │
                ▼
 ┌──────────────────────────────────────┐
 │      图片转换 (image_convert.c)      │
 │  1. 读取文件头魔数，判断 JPEG/PNG   │
 │  2. JPEG: TJpgDec 文件流式解码      │
 │     PNG: LodePNG 内存解码           │
 │  3. 按当前面板尺寸缩放/居中或识别预处理 PNG │
 │  4. RGB → 灰度 + 红色通道分离       │
 │  5. Floyd-Steinberg 误差扩散抖动    │
 │  6. 灰度 → 1bpp 黑色平面打包       │
 │  7. 输出: black[] + red[]/yellow[]  │
 │     → /spiffs/image.bin（按面板尺寸）│
 └──────────────┬───────────────────────┘
                │
                ▼
 ┌──────────────────────────────────────┐
 │        EPD 驱动 (epd_stub.c)        │
 │  1. 按 NVS 面板配置初始化控制器     │
 │  2. 按当前面板协议写黑色/彩色平面   │
 │  3. 触发稳定全刷新                  │
 │  4. 等待 BUSY 释放                  │
 └──────────────┬───────────────────────┘
                │ SPI DMA (4KB 分块)
                ▼
 ┌──────────────────────────────────────┐
 │  4.2" / 5.83" BWR 墨水屏 framebuffer │
 │    三色电子纸显示，断电保持画面      │
 └──────────────────────────────────────┘
```

### 3.4 Flash 分区表

```csv
# Name,      Type, SubType,  Offset,    Size

nvs,         data, nvs,      0x9000,    0xC000      # 48KB — 多模块配置 + PHY 校准
otadata,     data, ota,      0x15000,   0x2000      # 8KB — OTA 启动选择
phy_init,    data, phy,      0x17000,   0x1000      # 4KB
factory,     app,  factory,  0x20000,   0x300000    # 3MB — 出厂固件槽
ota_0,       app,  ota_0,    0x320000,  0x300000    # 3MB — OTA 分区 A
ota_1,       app,  ota_1,    0x620000,  0x300000    # 3MB — OTA 分区 B
coredump,    data, coredump, 0x920000,  0x20000     # 128KB — 崩溃转储
fontfs,      data, spiffs,   0x940000,  0x2E0000    # 2.875MB — 内置字库
spiffs,      data, spiffs,   0xC20000,  0x3E0000    # 3.875MB — 用户图片/画布/运行时文件
```

> 已实现 factory + 双 OTA 三槽布局，支持空中固件升级与回滚保护。当前分区表把系统崩溃日志、内置字库和用户文件分离：`coredump` 用于 panic / watchdog 复盘，`fontfs` 用于 16/24/32px MEF 字库，用户 `spiffs` 固定在 `0xC20000` 以减少分区调整对图库/配置的影响。分区表或 `fontfs` 地址变化后，发布包需要同时更新 `partition_table.bin` 与 `fontfs.bin`，不能只 OTA app。

---

## 四、核心技术实现

### 4.1 SSD1619 初始化序列详解

上电后 EPD 初始化在 `epd_init_sequence_ssd1619()` 中执行，完整序列如下：

```
步骤  命令    数据          含义
────  ────    ────          ────
 1    硬件复位               RES引脚拉低10ms再拉高10ms
 2    等待BUSY              等待控制器就绪

 3    0x12    无             软件复位(SWRESET)，清除内部状态
 4    等待BUSY              等待复位完成

 5    0x74    0x54           设置模拟块控制寄存器
                             配置内部模拟电路参数

 6    0x7E    0x3B           设置数字块控制寄存器
                             配置内部数字电路参数

 7    0x2B    0x04, 0x63     ACVCOM 设置（驱动电压相关）
                             配置 VCOM 交流驱动时序参数

 8    0x0C    0x8B, 0x9C,    Booster Soft Start 控制
              0x96, 0x0F     配置升压电路的软启动参数:
                             Phase A=0x8B, Phase B=0x9C,
                             Phase C=0x96, Duration=0x0F

 9    0x01    0x2B, 0x01,    Driver Output Control
              0x00           设置驱动输出行数：
                             0x012B = 299，即 300 行(0-299)
                             第3字节=0x00: 扫描方向=正常

10    0x11    0x01           Data Entry Mode Setting
                             0x01: X递增, Y递减
                             扫描方向：从左到右，从下到上

11    0x44    0x00, 0x31     Set RAM X Address Range
                             X起始=0x00(像素0), X结束=0x31(像素49*8-1=399→49)
                             地址单位=8像素(1字节), 覆盖400像素宽度

12    0x45    0x2B, 0x01,    Set RAM Y Address Range
              0x00, 0x00     Y起始=0x012B(299), Y结束=0x0000(0)
                             从底部到顶部扫描

13    0x3C    0x01           Border Waveform Control
                             0x01: 设置边框波形为固定电平

14    0x18    0x80           Temperature Sensor Control
                             0x80: 使用内部温度传感器
                             影响刷新波形的温度补偿

15    0x22    0xB1           Display Update Control 2
              0x20           Master Activation
              等待BUSY       执行首次激活序列(加载LUT波形表)
                             0xB1: 加载温度值+加载LUT

16    0x4E    0x00           Set RAM X Address Counter
                             X光标归零(第0列)

17    0x4F    0x2B, 0x01     Set RAM Y Address Counter
              等待BUSY       Y光标设为299(底部)
                             这决定了数据写入的起始位置
```

### 4.2 SPI DMA 批量传输

传统逐字节 SPI 传输在发送一个颜色平面时需要大量 SPI 事务，效率极低。本项目采用 **DMA 批量传输**，以 4096 字节为块发送。当前平面大小由 `epd_plane_bytes() = width × height / 8` 动态决定；4.2" 400×300 为 15,000 字节，5.83" 648×480 为 38,880 字节。

```c
// epd_stub.c — 批量数据传输
static void epd_send_data_bulk(const uint8_t *data, size_t len)
{
    gpio_set_level(PIN_EPD_DC, 1);    // D/C = 1 表示数据模式
    const size_t CHUNK = 4096;         // 每次传输 4KB
    size_t offset = 0;
    while (offset < len) {
        size_t to_send = (len - offset > CHUNK) ? CHUNK : (len - offset);
        spi_transaction_t t = {0};
        t.length = to_send * 8;        // 长度单位为 bit
        t.tx_buffer = data + offset;   // DMA 直接从缓冲区读取
        spi_device_transmit(s_spi, &t);
        offset += to_send;
    }
}
```

**关键点：**

- `SPI_DMA_CH_AUTO` 让驱动自动选择可用 DMA 通道
- `max_transfer_sz = 16384` 允许单次传输最大 16KB
- 实际使用 4KB 分块，是 DMA 效率和内存占用的平衡点
- 4.2" 的 15,000 字节平面仅需 4 次 SPI 事务；5.83" 的 38,880 字节平面约 10 次 SPI 事务
- 填充固定值时使用 `epd_fill_data()`，分配单个 4KB DMA 缓冲区重复发送

### 4.3 图片处理流水线

图片从用户选择到屏幕显示，经过完整的处理流水线：

#### 第一步：浏览器端预览与预处理

```javascript
// web/index.html — 预览 Canvas 导出
function buildUploadBlobFromPreview() {
    // 1. 将图片绘制到当前面板尺寸的预览 Canvas
    // 2. 应用方向、裁剪/适应、阈值、红色/黄色识别和抖动参数
    // 3. 使用 toBlob 输出为 PNG，再通过 /upload 上传
}
```

- **目的**：让用户在浏览器中先看到接近墨水屏的黑白红/黑白红黄效果，再上传已经按面板尺寸处理过的 PNG。
- **输出尺寸**：由 `epdPanelW` / `epdPanelH` 决定，来自设备当前面板配置；4.2" 为 400×300，5.83" 为 648×480。
- **优势**：截图、线稿、二维码和纯黑白图片可在浏览器端提前量化，固件侧识别预处理 PNG 后可走 passthrough/阈值路径，减少二次抖动导致的线条问题。

#### 第二步：JPEG 流式解码（TJpgDec 从文件读取）

```c
// image_convert.c — 文件流式输入回调
static UINT jpeg_infunc_file(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    if (buf) {
        return (UINT)fread(buf, 1, len, ctx->fp);  // 从文件读入解码器
    }
    // skip 模式：仅移动文件指针
    fseek(ctx->fp, (long)len, SEEK_CUR);
    return len;
}
```

- **TJpgDec** 是 ESP32 ROM 内置的轻量 JPEG 解码器
- 使用 **文件流式读取**（`jpeg_infunc_file`），避免将整个 JPEG 载入内存
- 工作池仅需 20KB（`POOL_SIZE = 20 * 1024`）
- 支持 1/1、1/2、1/4、1/8 四档缩放，按当前 `epd_width()` / `epd_height()` 选择适合面板目标尺寸的比例

**缩放策略：**

```c
// 从 scale=0(1/1) 到 scale=3(1/8) 逐级尝试
// 选择第一个使输出尺寸 ≤ 目标尺寸的缩放级别
for (uint8_t s = 0; s <= 3; s++) {
    int sw = (in_w + ((1 << s) - 1)) >> s;  // 向上取整除以 2^s
    int sh = (in_h + ((1 << s) - 1)) >> s;
    if (sw <= epd_width() && sh <= epd_height()) return s;
}
```

#### 第三步：灰度 + 红色分离

在 JPEG 解码的输出回调 `jpeg_outfunc` 中，对每个像素进行处理：

```c
// 红色检测：R>150, G<120, B<120, 且 R 比 max(G,B) 大 40 以上
static inline bool is_red_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t max_gb = (g > b) ? g : b;
    return (r > 150) && (g < 120) && (b < 120) && ((int)r - (int)max_gb > 40);
}

// 输出回调中：
if (is_red_pixel(r, g, b)) {
    red[byte_pos] |= bit;    // 标记红色位
    gray[idx] = 255;          // 红色区域灰度设为白色（不参与黑白抖动）
} else {
    uint8_t lum = (r * 30 + g * 59 + b * 11) / 100;  // BT.601 加权灰度
    gray[idx] = lum;
}
```

- 灰度使用 **BT.601 加权公式**：`Y = 0.30R + 0.59G + 0.11B`
- 红色像素标记后灰度设为白色，防止其参与黑白抖动
- 红色检测使用 **多维阈值**，避免棕色、橙色等被误判

#### 第四步：Floyd-Steinberg 抖动

对灰度缓冲区应用误差扩散抖动，在黑白二值的基础上模拟灰度层次：

```c
static void apply_floyd_steinberg(uint8_t *gray, const uint8_t *red, int w, int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // 跳过红色像素
            if (red[byte_pos] & mask) continue;

            int old_val = gray[idx];
            int new_val = (old_val < 128) ? 0 : 255;   // 二值化
            gray[idx] = new_val;
            int err = old_val - new_val;                 // 量化误差

            // 将误差扩散到相邻像素（Floyd-Steinberg 权重矩阵）
            //          [*]  7/16
            //   3/16  5/16  1/16
            if (x + 1 < w)           gray[idx + 1]     += err * 7 / 16;
            if (y + 1 < h) {
                if (x > 0)           gray[idx + w - 1] += err * 3 / 16;
                                     gray[idx + w]     += err * 5 / 16;
                if (x + 1 < w)       gray[idx + w + 1] += err * 1 / 16;
            }
        }
    }
}
```

**误差扩散权重矩阵图示：**

```
          当前像素   → 7/16
   3/16  ← 5/16  → 1/16
```

#### 第五步：1bpp MSB-first 打包

抖动后的灰度值转为 1-bit 黑色平面：

```c
static void pack_gray_to_black(const uint8_t *gray, uint8_t *black, int w, int h)
{
    memset(black, 0xFF, plane_bytes);  // 默认全白（1=白）
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (gray[y * w + x] < 128) {
                size_t byte_pos = y * (w / 8) + (x >> 3);
                uint8_t bit = 0x80 >> (x & 7);    // MSB-first: 最左像素在最高位
                black[byte_pos] &= ~bit;           // 清零=黑色
            }
        }
    }
}
```

### 4.4 三色数据格式

主测 BWR 面板使用黑色平面和彩色平面存储三色数据；命令、极性和刷新序列由 `epd_stub.c` 的面板描述表决定。SSD1619 4.2" 默认使用 `0x24/0x26`，UC8179 5.83" 路径使用 `0x10/0x13`。

| 平面 | 写入命令 | 大小 | 编码 |
|------|---------|------|------|
| **黑色平面 (BW RAM)** | 依面板而定，SSD1619 为 `0x24` | `width × height / 8` 字节 | 通常 `1` = 白色, `0` = 黑色 |
| **彩色平面 (RED/YELLOW RAM)** | 依面板而定，SSD1619 红色为 `0x26` | `width × height / 8` 字节 | 按面板驱动极性映射红/黄 |

**像素颜色决定规则：**

| 黑色平面 (0x24) | 红色平面 (0x26) | 显示颜色 |
|:---:|:---:|:---:|
| 1 | 0 | 白色 |
| 0 | 0 | 黑色 |
| × | 1 | **红色** |

**位序：MSB-first**

每个字节的最高位对应最左像素：

```
字节 0x00 处:  bit7=像素(0,y)  bit6=像素(1,y)  ...  bit0=像素(7,y)
字节 0x01 处:  bit7=像素(8,y)  bit6=像素(9,y)  ...  bit0=像素(15,y)
```

**写入顺序：先设光标再选 RAM**

```c
// SSD1619 4.2" 示例；其它面板由 epd_stub.c 的 panel_desc 路径选择命令和极性
epd_set_cursor_home();       // 0x4E=0x00, 0x4F=0x012B（光标到左下角）
epd_send_command(0x24);      // 选择黑色 RAM
epd_send_data_bulk(black, epd_plane_bytes());

epd_set_cursor_home();       // 再次归零光标（每次写 RAM 后光标已移动）
epd_send_command(0x26);      // 选择红色 RAM
epd_send_data_bulk(red, epd_plane_bytes());

epd_refresh_full();          // 0x22=0xC7, 0x20 触发刷新
```

### 4.5 内存优化策略

ESP32-S3 内部 SRAM 有限；在无 PSRAM 机型上图片处理需要精细控制峰值，在 N16R8 等带 PSRAM 机型上大图解码和 5.83" framebuffer 可优先落到 PSRAM：

| 数据 | 大小 | 生命周期 |
|------|------|----------|
| 灰度缓冲区 `gray[]` | 约 `width × height` 字节；4.2" 为 120,000 字节，5.83" 为 311,040 字节 | 解码→抖动→打包后释放 |
| 彩色平面 `red[]` / `yellow[]` | 每平面 `width × height / 8` 字节；4.2" 为 15,000 字节，5.83" 为 38,880 字节 | 解码→抖动→写文件后释放 |
| 黑色平面 `black[]` | 每平面 `width × height / 8` 字节 | 打包→写文件后释放 |
| TJpgDec 工作池 | 20,480 字节 | 解码期间 |
| EPD DMA / 行缓冲 | 约 4KB DMA 块或按行临时缓冲 | 文件读取→SPI 传输后释放；实际大小由 `epd_stub.c` 的发送路径决定 |
| 总峰值 | 随面板尺寸和图片格式变化 | 4.2" 黑白红约 170KB 量级；5.83"、PNG 解码和四色路径建议使用带 PSRAM 的 N16R8 |

**关键优化措施：**

1. **JPEG 流式解码**：`jpeg_infunc_file` 从文件流式读取，不需要将整个 JPEG 载入内存
2. **分阶段释放**：灰度缓冲区在打包为 1bpp 后立即释放；抖动和打包共享灰度缓冲区
3. **DMA 缓冲区复用**：`epd_fill_data` 分配单个 4KB 缓冲区，填充后重复发送
4. **EPD raw 文件中转**：转换结果先写入 SPIFFS，raw 大小按 `epd_plane_bytes()` 和颜色平面数动态变化；EPD 驱动再从文件读取，避免同时持有多份完整平面数据
5. **LodePNG 编译裁剪**：禁用编码器、磁盘 I/O、辅助块、C++ 支持，减小代码体积

```cmake
# main/CMakeLists.txt — LodePNG 编译优化

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LODEPNG_NO_COMPILE_ENCODER=1
    LODEPNG_NO_COMPILE_DISK=1
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS=1
    LODEPNG_NO_COMPILE_CPP=1
)
```

### 4.6 PNG 解码路径

PNG 文件需要整体读入内存后由 LodePNG 解码为 RGBA，再进行缩放和颜色处理：

```c
// 1. 读取整个 PNG 文件到内存
uint8_t *buf = malloc(file_size);
fread(buf, 1, file_size, pf);

// 2. LodePNG 解码为 RGBA
unsigned char *rgba = NULL;
lodepng_decode32(&rgba, &pw, &ph, buf, got);
free(buf);  // 立即释放 PNG 原始数据

// 3. 计算缩放比例（保持宽高比）
int ew = epd_width();
int eh = epd_height();
float scale = max(pw/ew, ph/eh, 1.0);

// 4. 最近邻缩放 + 居中 + 红色分离 + 灰度
// 5. Alpha 通道合成（透明区域与白色背景混合）
if (a < 255) {
    r = (r * a + 255 * (255 - a)) / 255;
    // ...
}

// 6. Floyd-Steinberg 抖动 → 1bpp 打包 → 写文件
```

> **注意**：PNG 路径内存需求较高（RGBA 占 4×宽×高 字节）。当前上传页会把图片先渲染到设备面板尺寸后再上传 PNG；若后续支持更大屏幕或高分辨率原图直传，需要重新评估 PNG 大小限制和 PSRAM 需求。

---

## 五、Web 界面功能

### 5.1 整体架构

Web 前端为 **多页面**：`index.html`（首页/上传）、`config.html`（综合配置）、`gallery.html`、`weather.html`、`clock.html`、`calendar.html`、`message.html`、`codex.html`、`timetable.html`、`todo.html`、`countdown.html`、`board.html`（画布编辑器）共 12 个 HTML；另嵌入 `miaooaim-mark.png` 品牌资源。构建时经 `main/CMakeLists.txt` 的 `EMBED_FILES` 嵌入镜像，由 `http_app` / `http_features` / `http_canvas` 按路径返回；运行时通过 `extern const uint8_t[]` 从 Flash 读取发送。

```c
// 编译时嵌入
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// HTTP 响应
httpd_resp_send(req, (const char *)index_html_start,
                index_html_end - index_html_start);
```

### 5.2 界面布局

采用暗色调赛博朋克风格设计，两栏响应式布局（移动端自动切换单栏）：

| 区域 | 功能 |
|------|------|
| **顶部标题栏** | 项目名称 + ESP32 连接提示胶囊 |
| **左侧：上传卡片** | 文件选择、上传按钮、操作日志 |
| **右侧：状态卡片** | upload.jpg 大小、image.bin 大小、SPIFFS 用量进度条、刷新/清屏按钮 |
| **底部：画廊卡片** | 网格瀑布流显示已存储图片，支持预览、显示、删除 |

### 5.3 上传流程

```
用户选择图片 → 按当前面板尺寸生成预览 Canvas
  → 应用阈值/对比度/红色或黄色识别/抖动
  → toBlob('image/png')
  → fetch('/upload', {method:'POST', body:blob})
  → 服务器接收 → 保存到 SPIFFS → 转换 → EPD 显示
  → 自动刷新状态和画廊
```

### 5.4 画廊管理

- **加载**：`GET /images` 获取 JSON 文件列表
- **预览**：`GET /image?name=xxx` 通过带认证的 `fetch()` 转为 Blob URL 后懒加载缩略图
- **显示**：`POST /show?name=xxx` 将指定图片转换并推送到墨水屏
- **删除**：`POST /delete_image?name=xxx` 从 SPIFFS 删除单张图片

### 5.5 设备状态监控

通过 `GET /status` 定时获取：

```json
{
    "upload_path": "/spiffs/upload.jpg",
    "upload_bytes": 85432,
    "raw_path": "/spiffs/image.bin",
    "raw_bytes": 30000,
    "spiffs_ok": true,
    "spiffs_total": 4063232,
    "spiffs_used": 425984
}
```

上例中的 `raw_bytes=30000` 是 4.2" 400×300 黑白红双平面的典型值；实际值随面板尺寸和颜色平面数变化。SPIFFS 使用率进度条表示 **用户文件区已用比例**，不是剩余比例；当前默认用户 SPIFFS 为 3.875MB。`fontfs` 为独立 2.875MB 字库分区，不进入这个进度条。颜色自适应：<70% 绿色、70~90% 黄色、>90% 红色。

### 5.6 HTTP API 端点

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/` | 主页（上传/画廊/状态）|
| GET | `/config` | 配置页（WiFi/轮播/天气/OTA/低功耗）|
| GET | `/timetable` | 课程表编辑页 |
| GET | `/todo` | 待办事项管理页 |
| GET | `/countdown` | 倒计时配置页 |
| POST | `/upload` | 图片上传 → 转换 → 显示 |
| GET | `/status` | 设备状态 JSON（启用 Basic Auth 后需认证） |
| GET | `/images` | 画廊列表 JSON（启用 Basic Auth 后需认证） |
| GET | `/image?name=xxx` | 图片预览（启用 Basic Auth 后需认证） |
| POST | `/show?name=xxx` | 显示指定图片 |
| POST | `/delete_image?name=xxx` | 删除单张图片 |
| POST | `/delete` | 清屏 |
| GET/POST | `/wifi_*`, `/scan` | WiFi 配网 API |
| GET/POST | `/panel_config` | 屏幕型号配置；保存后需重启生效 |
| GET/POST | `/weather_*` | 天气配置 API |
| GET/POST | `/clock_*` | 时钟配置 API |
| GET/POST | `/slideshow` | 轮播配置 API |
| GET/POST | `/timetable*` | 课程表数据 API |
| GET/POST | `/todo*` | 待办事项数据 API |
| GET/POST | `/countdown*` | 倒计时数据 API |
| GET/POST | `/msg_*` | 留言板配置 API |
| GET/POST | `/board`, `/canvas_*` | 画布留言板可视化编辑与 API |
| POST | `/calendar_show` | 显示日历 |
| GET/POST | `/power_config` | 电源管理配置 API |
| GET/POST | `/auth_config` | HTTP 认证配置 |
| POST | `/ota` | 固件升级 |
| GET | `/version` | 固件版本 |
| POST | `/spiffs_remount` / `/spiffs_format` | 文件系统诊断恢复；格式化必须提交 `{"confirm":"FORMAT_SPIFFS"}` |
| *… 共 73 条路由，以 `http_app.c` 为准* | | |

---

## 六、关键问题与解决方案

### 6.1 SPIFFS 文件系统健康检查与数据保护

**问题**：ESP32 异常断电或频繁写入后，SPIFFS 可能损坏，导致文件读写失败，甚至挂载失败。

**解决方案**（`spiffs_mount.c`）：

```text
挂载 SPIFFS
  ├─ 成功 → 写读验证（写入"SPIFFS_OK"并读回比对）
  │          ├─ 通过 → 正常使用
  │          └─ 失败 → 仅记录告警，保留现有文件系统
  └─ 失败 → 返回错误并保留用户数据；正常启动继续保留 AP/Web 诊断能力
```

- 使用快速的写读验证替代耗时的 `esp_spiffs_check()`（后者可能花费数十秒）
- 当前版本已改为 `format_if_mount_failed=false`；配置页提供“重试文件系统”和“格式化 SPIFFS”入口，格式化必须输入确认口令，避免误清空图库和画布素材
- 挂载失败的完整启动路径会先启动 HTTP，再尝试初始化 EPD；若屏幕可用，会显示中文 `文件系统恢复` 页面，包含 AP 热点名、默认密码 `12345678`、`192.168.4.1/config` 和 `FORMAT_SPIFFS` 确认词，避免用户在诊断模式下找不到恢复入口
- 若屏幕型号错误或屏幕完全不显示，用户仍可直接连接 `ESP32_EPD_xxxxxx` 热点，默认密码为 `12345678`，用于进入网页切换面板或恢复配置
- 挂载成功后自动创建画廊目录 `/spiffs/images`

### 6.2 内存不足：从内存缓冲改为文件流式解码

**问题**：最初实现将整个 JPEG 文件读入内存后再解码，在处理 100KB+ 的 JPEG 时，加上解码输出缓冲区（灰度 120KB + 红色 15KB + 黑色 15KB + 工作池 20KB），总计超过 270KB，超出可用 RAM。

**解决方案**：

1. 实现 `jpeg_infunc_file` 回调，让 TJpgDec 直接从文件流式读取
2. 不再需要 JPEG 原始数据的内存缓冲
3. 4.2" BWR 典型峰值降至约 170KB 量级；5.83"、PNG 和四色面板路径需按实际面板尺寸重新估算

```c
// 对比两种路径
// 旧：内存路径（需要额外分配 JPEG 大小的缓冲区）
uint8_t *jpeg_buf = malloc(file_size);  // 可能 100~200KB！
fread(jpeg_buf, 1, file_size, f);
image_convert_jpeg_to_epd_raw(jpeg_buf, file_size, out);

// 新：文件流式路径（零额外缓冲）
image_convert_file_to_epd_raw(input_path, out_path);
// 内部：TJpgDec 按需从文件读取小块数据
```

### 6.3 HTTP 连接管理

**问题**：ESP32 HTTP 服务器资源有限，手机浏览器可能保持多个空闲连接，导致新连接被拒绝。

**解决方案**（`http_app.c`）：

```c
httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.recv_wait_timeout  = 5;      // 接收超时 5 秒，快速释放空闲连接
config.send_wait_timeout  = 5;      // 发送超时 5 秒
config.max_uri_handlers   = 80;     // esp_http_server 可注册句柄上限（须 ≥ 实际 URI 条数；当前 73）
config.stack_size         = 16384;  // HTTP 任务栈 16KB（图片转换需要较深调用栈）
config.lru_purge_enable   = true;   // 当连接数满时，关闭最久未使用的连接
```

- **LRU Purge**：当达到最大连接数时，自动关闭最久未活动的连接，为新请求腾出资源
- **超时设置**：5 秒超时防止僵尸连接长期占用
- **栈大小 16KB**：图片转换路径调用栈较深（HTTP handler → image_convert → TJpgDec → 回调），默认 4KB 不够

### 6.4 位序问题：MSB-first vs LSB-first

**问题**：SSD1619 的 RAM 地址映射中，每个字节的 bit7 对应最左边的像素（MSB-first）。初期误用 LSB-first 位序导致图案水平镜像。

**解决方案**：

```c
// 正确：MSB-first — bit7 = 最左像素
uint8_t bit = 0x80 >> (x & 7);    // x=0 → bit7, x=1 → bit6, ...
black[byte_pos] &= ~bit;           // 清除位 = 黑色

// 错误：LSB-first — 会导致每 8 像素一组镜像
uint8_t bit = 1 << (x & 7);       // 千万不要这样写！
```

### 6.5 RAM 写入命令顺序

**问题**：SSD1619 要求在发送图像数据前，必须先用 `0x4E`/`0x4F` 设置 RAM 地址计数器（光标位置），然后再用 `0x24`/`0x26` 选择目标 RAM 并开始写入。顺序错误会导致数据写入错误位置。

**解决方案**：严格遵循"先设光标再选 RAM"的顺序，且每写完一个平面后都需要重新设置光标。

```c
epd_set_cursor_home();     // ① 先设光标
epd_send_command(0x24);    // ② 再选黑色 RAM
epd_send_data_bulk(black, epd_plane_bytes());  // ③ 写数据

epd_set_cursor_home();     // ④ 写完黑色后光标已移动，必须重新设
epd_send_command(0x26);    // ⑤ 选红色 RAM
epd_send_data_bulk(red, epd_plane_bytes());    // ⑥ 写数据
```

### 6.6 引脚映射匹配物理接线

**问题**：ESP32-S3 GPIO 编号与开发板丝印、墨水屏排线对应关系容易搞错。例如将 BUSY 接到了没有上拉的引脚上，导致 `epd_wait_busy()` 永远返回忙。

**解决方案**：

- BUSY 引脚配置为输入并启用内部上拉（`pull_up_en = 1`）
- 代码中集中定义引脚宏，确保与硬件接线一致：
  ```c
  #define PIN_EPD_SCK   4
  #define PIN_EPD_MOSI  5
  #define PIN_EPD_DC    7
  #define PIN_EPD_CS    15
  #define PIN_EPD_RST   6
  #define PIN_EPD_BUSY  16
  ```
- BUSY 等待设置 60 秒超时保护，防止硬件异常时程序挂死

### 6.7 浏览器缓存

**问题**：浏览器会缓存 HTML 页面和 API 响应，导致固件更新后用户看到旧版本界面，或状态数据不实时。

**解决方案**：

```c
httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
```

对主页 HTML 设置禁止缓存的 HTTP 头，确保每次访问都获取最新内容。

### 6.8 清屏的双刷新问题

**问题**：三色墨水屏清屏需要特殊处理。如果仅写入白色数据一次刷新，红色残影可能残留。

**解决方案**：清屏使用两次完整刷新周期：

```c
static void epd_clear(void)
{
    // 第一遍：黑色平面全白(0xFF) + 红色平面全零(0x00) → 刷新
    // 清除红色残影
    epd_set_cursor_home();
    epd_send_command(0x24); epd_fill_data(0xFF, epd_plane_bytes());
    epd_set_cursor_home();
    epd_send_command(0x26); epd_fill_data(0x00, epd_plane_bytes());
    epd_refresh_full();

    // 第二遍：两个平面都全白(0xFF) → 刷新
    // 确保完全清屏
    epd_set_cursor_home();
    epd_send_command(0x24); epd_fill_data(0xFF, epd_plane_bytes());
    epd_set_cursor_home();
    epd_send_command(0x26); epd_fill_data(0xFF, epd_plane_bytes());
    epd_refresh_full();
}
```

---

## 七、构建与烧录

### 7.1 环境要求

| 组件 | 版本 |
|------|------|
| ESP-IDF | **最低 v5.5.1**（5.5.x 更高一般兼容） |
| CMake | ≥ 3.16 |
| Python | ≥ 3.8 |
| 操作系统 | Windows / macOS / Linux |

### 7.2 构建命令

```bash
#cd 到源码目录
# 设置目标芯片

idf.py set-target esp32s3

# 编译

idf.py build
```

### 7.3 烧录命令

```bash
# 烧录固件

idf.py -p COMx flash

# 查看串口日志

idf.py -p COMx monitor

# 一步完成编译+烧录+监控

idf.py flash monitor
```

> 将 `COMx` 替换为实际的串口号，如 `COM3`（Windows）或 `/dev/ttyUSB0`（Linux）。

### 7.4 何时需要 erase-flash

日常升级不要求先擦除整颗 Flash。只有在分区表变更、Flash 内容混乱、需要完全恢复出厂状态，或用户明确接受清空 WiFi/天气/面板配置和 SPIFFS 用户文件时，才建议执行：

```bash
idf.py -p COMx erase-flash
idf.py -p COMx flash
```

**原因**：`erase-flash` 会清空 NVS、用户 SPIFFS、fontfs、app 和所有运行时数据。当前版本 `format_if_mount_failed=false`，SPIFFS 挂载失败时不会自动格式化用户内容；正常启动会保留 AP/Web 恢复入口，必要时由用户在配置页确认后再格式化。

---

## 八、使用指南

### 8.1 连接 WiFi

1. ESP32-S3 上电后，自动创建 WiFi 热点
2. 在手机/电脑的 WiFi 列表中找到 **ESP32_EPD_xxxxxx**
3. 输入默认热点密码 **12345678** 连接
4. 连接成功后，打开浏览器访问 **http://192.168.4.1/**

### 8.2 上传图片

1. 点击「选择文件」按钮，选择一张 JPEG、PNG 或 BMP 图片
2. 点击「上传并显示」按钮
3. 浏览器按当前面板尺寸生成预处理 PNG
4. 上传到 ESP32 后，系统自动进行：
   - 按文件魔数保存画廊副本（`.png` / `.bmp` / `.jpg`）
   - 固件识别 JPEG / PNG / BMP 后转换为当前面板 raw 平面
   - 通过 SPI DMA 传输到墨水屏
5. 等待墨水屏完成全刷新；实际耗时随 4.2"/5.83" 面板、温度和 BUSY 状态变化，常见约 10~35 秒

### 8.3 画廊管理

- **浏览**：页面底部画廊区域显示所有已存储的图片缩略图
- **显示**：点击图片下方的「显示」按钮，将该图片推送到墨水屏
- **删除**：点击「删除」按钮移除单张图片，释放 SPIFFS 空间
- **清屏**：点击状态区域的「删除文件并清屏」，清除临时文件并将墨水屏清为白色

### 8.4 设备状态

- **upload.jpg**：最近一次上传的临时文件路径名；历史上沿用 `.jpg` 后缀，实际内容可能是浏览器预处理后的 PNG
- **image.bin**：最近一次转换的 EPD raw 文件，大小随面板尺寸和颜色平面数变化；4.2" BWR 典型为 30000 字节
- **SPIFFS**：文件系统总容量和已用空间，进度条直观显示使用率

### 8.5 注意事项

- 墨水屏断电后画面保持，无需持续供电
- 三色墨水屏全刷新耗时随面板、温度和 BUSY 状态变化，常见约 10~35 秒，请耐心等待
- 红色识别对颜色纯度有要求（R>150, G<120, B<120），偏橙或偏棕的颜色可能不显示为红色
- SPIFFS 剩余空间不足时，请删除画廊中不需要的图片
- 出现异常时，尝试重新上电或通过串口执行 `erase-flash` 后重新烧录

---

## 附录 A：初始化时序图

```
app_main()
    │
    ├─ nvs_flash_init()              NVS 键值存储初始化
    │   └─ 若版本不匹配 → erase + 重新初始化
    │
    ├─ spiffs_mount_init()           SPIFFS 文件系统
    │   ├─ 挂载（失败返回错误，保留用户数据，不自动格式化）
    │   ├─ 写读验证（失败仅告警，保留文件系统）
    │   └─ 创建 /spiffs/images/ 目录
    │
    ├─ power_mgr_init()              电源管理初始化（检测唤醒原因）
    │
    ├─ [定时器唤醒?]
    │   └─ YES → quick_refresh_and_sleep()（快速刷新 → 回睡）
    │   └─ NO  → full_boot()（完整启动流程）
    │
    full_boot():
    ├─ epd_init()                    墨水屏驱动（失败时保留 AP/Web 诊断入口）
    │   ├─ GPIO 配置（DC/CS/RST=输出，BUSY=输入上拉）
    │   ├─ SPI 总线初始化（SPI2_HOST, DMA, 2MHz）
    │   └─ SSD1619/UC8179 初始化序列
    │
    ├─ wifi_manager_start()          WiFi AP+STA 启动
    │   ├─ AP 模式始终开启
    │   └─ 有 NVS 凭据则自动连接 STA
    │
    ├─ mDNS + SNTP                   局域网发现 + 时间同步
    │
    ├─ 内容模块初始化                天气/时钟/日历/课程表/待办/留言板/画布/轮播
    │
    ├─ http_app_start()              HTTP 服务器（73 条路由，Basic Auth）
    │
    ├─ 显示欢迎屏
    │
    └─ power_mgr_arm()               启动闲置监控（超时 → 深度睡眠）
```

## 附录 B：EPD 刷新命令速查

| 命令 | 名称 | 参数 | 用途 |
|------|------|------|------|
| `0x12` | SW Reset | 无 | 软件复位 |
| `0x01` | Driver Output | 3B | 设置驱动行数和扫描方向 |
| `0x11` | Data Entry Mode | 1B | X/Y 递增递减方向 |
| `0x44` | RAM X Range | 2B | X 地址范围 (0x00~0x31) |
| `0x45` | RAM Y Range | 4B | Y 地址范围 (299~0) |
| `0x4E` | RAM X Counter | 1B | 设置 X 写入光标 |
| `0x4F` | RAM Y Counter | 2B | 设置 Y 写入光标 |
| `0x24` | Write BW RAM | nB | 写入黑白数据平面 |
| `0x26` | Write RED RAM | nB | 写入红色数据平面 |
| `0x22` | Display Update Ctrl | 1B | 选择刷新序列 |
| `0x20` | Master Activation | 无 | 触发刷新 |
| `0x3C` | Border Waveform | 1B | 边框波形控制 |
| `0x18` | Temp Sensor | 1B | 温度传感器选择 |
| `0x0C` | Booster Soft Start | 4B | 升压电路软启动参数 |

---

*本文档基于项目当前代码生成，随开发迭代持续更新。*
