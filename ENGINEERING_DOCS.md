# 喵哎-MiaooAim 工程技术文档

## 工程状态（2026-06-28）

本文档反映当前 ESP32-S3 三色墨水屏固件的工程实现状态。已完成显示 epoch 仲裁、日历/画布/留言异步显示返回语义收紧、GET 数据/媒体接口 Basic Auth、SPIFFS 用户数据保护与挂载失败降级启动/手动恢复入口、配置备份排除 Basic Auth、面板配置重启生效、图库坏图清理、手动画面期间天气后台收敛、EPD 初始化失败降级诊断、AP 密码日志脱敏、外置 fontfs 字库、低功耗按模式快速刷新、日历跨日刷新、Web 画板字体渲染和 5.83" 页面/驱动说明同步。

2026-06-24 完成公开发布目录整理与一致性检查：构建缓存、SDK 配置、生成镜像和工具缓存等可再生成内容不随仓库发布，保留 release、字库下载/生成脚本和必要参考素材；TTF 字体缓存由构建脚本按需下载到本地 ignored 目录；发布检查已覆盖 HTTP 路由、Web 嵌入资源、分区表、字库授权和 release 烧录说明；`release/README.md` 已修正 fontfs 烧录地址为 `0x940000`。

2026-06-25 完成文档一致性补充：低功耗开关不关闭正常启动后台，只有 RTC 定时器唤醒才进入 quick-refresh；5.83" BWR 供应商标注兼容微雪 5.83b V2 时优先选择 `Waveshare 5.83" BWR B V2`；Web 嵌入资源为 12 个 HTML 页面 + 1 个 PNG；用户 SPIFFS 与 fontfs 分区职责分离。

2026-06-28 同步 v2.3.4 版本准备：补充 UC 4.2 三色与 SSD1619A 4.2 双色参考驱动选项，并收紧低功耗闲置计时，避免后台状态轮询和自动刷屏持续阻止深睡。

建议复刻和二次开发时重点验证 OTA 成功/失败/回滚流程、低功耗唤醒、轮播与天气长时间运行、连续上传抢占场景，以及实际屏幕驱动 IC 和 BUSY 电平。发布新固件前必须在 ESP-IDF 终端重新执行完整 `idf.py build`。

> **项目版本**: v2.3.4（`CMakeLists.txt` → `PROJECT_VER` → `GET /version` 中 `version`）
> **文档版本**: v2.3.4-docs（公开发布同步版）
> **最后更新**: 2026-06-28
> **硬件平台**: ESP32-S3 + SSD1619(4.2") / UC8179(5.83")，并接入 ATC/Solum 与 EPD-nRF5 参考实验面板配置入口
> **软件框架**: ESP-IDF（**最低要求 v5.5.1**；5.5.x 更高发行版通常兼容）
> **文档状态**: 当前公开发布文档

---

## 目录

1. [项目概述](#一项目概述)
2. [系统架构](#二系统架构)
3. [硬件设计](#三硬件设计)
4. [软件架构](#四软件架构)
5. [模块详细说明](#五模块详细说明)
6. [API 接口文档](#六 api 接口文档)
7. [Web 前端文档](#七 web 前端文档)
8. [构建与部署](#八构建与部署)
9. [故障排查](#九故障排查)
10. [附录](#十附录)

---

## 一、项目概述

### 1.1 项目简介

喵哎-MiaooAim 是一款基于 ESP32-S3 的低功耗三色墨水屏智能终端，支持黑白红三色显示。项目集成 WiFi 配网、图片画廊、天气、时钟、日历、课程表、待办、倒计时、留言板、画板、Codex 额度看板、OTA 和低功耗等功能，可通过 Web 界面进行配置和管理。

### 1.2 核心特性

- **多面板支持**：主测 4.2" (400×300) 和 5.83" (648×480) 三色墨水屏；配置页另接入 ATC/Solum 与 EPD-nRF5 参考实验面板。
- **WiFi 连接**：STA/AP 模式，mDNS 发现，自动重连。
- **图片显示**：JPEG/PNG/BMP 格式，Floyd-Steinberg 抖动算法。
- **Web 管理**：响应式 UI，三套主题，OTA 升级。
- **内容页面**：天气、时钟、日历、课程表、待办、倒计时、Codex 额度看板、留言板、WYSIWYG 画布。
- **低功耗**：深度睡眠，定时器/按键唤醒；续航需按实际电池、屏幕、LDO、唤醒间隔和联网时长估算。
- **物理交互**：三键循环切换 **8 种**已注册 `display_mode`（时钟/日历/课程表/天气/轮播/待办/倒计时/额度）；**留言板与画布**由网页推屏，不参与按键轮显。

### 1.3 技术栈

| 层级 | 技术 | 版本 |
|------|------|------|
| 硬件 | ESP32-S3 **N16R8（8MB PSRAM）为主力**；N16 无 PSRAM 为降级兼容目标 | 16MB Flash；`sdkconfig.defaults` 启用 PSRAM 且允许缺失，N16 发布前仍需按大图/网页并发场景实测 |
| 框架 | ESP-IDF | **最低 v5.5.1**（推荐同系列 5.5.x） |
| 驱动 | SSD1619 / UC8179 | 原厂驱动 |
| 文件系统 | SPIFFS | `fontfs` 2.875MB + 用户 `spiffs` 3.875MB |
| 网络 | WiFi 802.11 b/g/n | SoftAP + STA |
| 协议 | HTTP/1.1, SPI, I2C | - |
| 前端 | HTML5/CSS3/JavaScript | 原生实现 |

### 1.4 性能指标

| 指标 | 数值 | 备注 |
|------|------|------|
| 图片上传→转换 | 场景相关 | 取决于图片尺寸、格式、PSRAM、抖动模式和 SPIFFS 写入状态 |
| 墨水屏全刷完成 | 约 10-35 秒 | 取决于面板型号、温度、BUSY 电平和波形；4.2"/5.83" 三色屏均以稳定全刷为当前默认路径 |
| WiFi 连接时间 | 场景相关 | 取决于路由器、信号强度、认证方式和重连状态 |
| 待机功耗 | 需按硬件实测 | 深度睡眠电流受 LDO、充电 IC、屏幕转接板、USB 芯片和 GPIO 漏电影响 |
| 工作电流 | 150-300mA | WiFi+ 刷新 |
| 续航时间 | 按电池和唤醒频率估算 | 需结合真实深睡电流、刷新耗时、天气联网耗时和电池容量计算 |
| 堆内存空闲 | 因场景波动 | N16R8 有 8MB PSRAM 兜底大分配；N16 无 PSRAM 才需控制 JPEG/PNG 峰值，见 `IMPLEMENTATION.md` |

---

## 二、系统架构

### 2.1 硬件架构

```
┌─────────────────────────────────────────────────────┐
│                  ESP32-S3-WROOM-1U                   │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │
│  │   CPU Dual  │  │  WiFi/BT    │  │ Flash 16MB  │  │
│  │   Core 240M │  │  802.11n    │  │ PSRAM 8MB   │  │
│  └─────────────┘  └─────────────┘  └─────────────┘  │
│         │                │                 │         │
│  ┌──────▼────────────────▼─────────────────▼──────┐  │
│  │              System Bus                         │  │
│  └──┬──────────┬──────────┬──────────┬──────────┬─┘  │
│     │          │          │          │          │    │
│  ┌──▼──┐  ┌────▼───┐  ┌──▼────┐  ┌─▼────┐  ┌─▼───┐ │
│  │ SPI │  │  USB   │  │  I2C  │  │ GPIO │  │ ADC │ │
│  │ EPD │  │  UART  │  │ NVS   │  │ BTN  │  │ PWR │ │
│  └─────┘  └────────┘  └───────┘  └──────┘  └─────┘ │
└─────────────────────────────────────────────────────┘
     │            │          │        │       │
┌────▼────┐  ┌───▼───┐  ┌──▼──┐  ┌─▼──┐  ┌─▼────┐
│  EPD    │  │USB-C  │  │NVS  │  │按键│  │电源  │
│ 4.2/5.83│  │CH340  │  │Flash│  │×3  │  │管理  │
└─────────┘  └───────┘  └─────┘  └────┘  └──────┘
```

### 2.2 软件架构

```
┌─────────────────────────────────────────────────────┐
│              Web Frontend (HTML/JS)                  │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────────┐  │
│  │  主页  │ │  配置  │ │课程表  │ │  待办事项    │  │
│  └───┬────┘ └───┬────┘ └───┬────┘ └─────┬────────┘  │
└──────┼──────────┼──────────┼────────────┼───────────┘
       │          │          │            │
       │  HTTP REST API (JSON)           │
       │          │          │            │
┌──────▼──────────▼──────────▼────────────▼───────────┐
│              HTTP Server (http_app.c)                │
│          73 Routes + Basic Auth + OTA                │
└────┬──────────┬──────────┬──────────┬──────────────┘
     │          │          │          │
┌────▼───┐ ┌───▼────┐ ┌───▼─────┐ ┌─▼────────────┐
│ 图片   │ │ WiFi   │ │ 业务    │ │ 系统管理     │
│ 处理   │ │ 管理   │ │ 逻辑    │ │              │
└────┬───┘ └───┬────┘ └───┬─────┘ └─┬────────────┘
     │          │          │         │
┌────▼──────────▼──────────▼─────────▼──────────────┐
│              Hardware Abstraction Layer            │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────────┐  │
│  │  EPD   │ │ SPIFFS │ │  NVS   │ │  Power     │  │
│  │ Driver │ │  FS    │ │ Storage│ │  Manager   │  │
│  └────────┘ └────────┘ └────────┘ └────────────┘  │
└─────────────────────────────────────────────────────┘
```

### 2.3 数据流

```
用户操作
   │
   ▼
Web UI / 按键输入
   │
   ▼
HTTP Server / Button Handler
   │
   ▼
业务逻辑处理 (Weather/Clock/Todo/Timetable)
   │
   ▼
数据持久化 (NVS/SPIFFS)
   │
   ▼
显示渲染 (Framebuffer)
   │
   ▼
EPD 驱动刷新
   │
   ▼
墨水屏显示
```

---

## 三、硬件设计

本仓库负责 ESP32-S3 固件、Web 管理端与烧录文档；配套 4.2" SSD1619 PCB、外壳附件与整机结构已开源到嘉立创开源硬件工程：[喵哎-MiaooAim 4.2寸 墨水屏 SSD1619](https://oshwhub.com/team_voosogmo/project_fxbcjhaa)。固件侧的引脚、屏幕配置、分区表和烧录流程以本仓库 README / `partitions.csv` 为准。

### 3.1 核心元器件

#### ESP32-S3 模组（与 `partitions.csv` 匹配）

**Flash 容量：** 本仓库分区表约需 **16MB** 级 Flash（factory + 双 OTA 各 3MB、128KB coredump、2.875MB fontfs、3.875MB 用户 SPIFFS）。**仅 8MB Flash（如部分 N8 模组）无法直接用于当前默认分区布局**，需改分区表后再谈兼容。

**PSRAM：** 主力硬件为 **N16R8（8MB PSRAM）**，`sdkconfig.defaults` 已开启 `CONFIG_SPIRAM` 和 `CONFIG_SPIRAM_IGNORE_NOTFOUND`。带 PSRAM 机型的大分配（PNG/JPEG 解码、双平面帧缓冲）可走 PSRAM；无 PSRAM（N16）作为降级兼容目标，发布前应按大图、5.83"、网页并发和画板场景单独实测。

**示例模组名：** `ESP32-S3-WROOM-1U-N16`、`ESP32-S3-WROOM-1U-N16R8`（具体以采购料号为准）。

**规格参数（典型）:**
- CPU: 双核 Xtensa LX7, 240MHz
- WiFi: 802.11 b/g/n (2.4GHz)
- Bluetooth: 5.0 LE（视模组是否裁切）
- 封装：SMD 模组

**应用引脚分配（已接入面板共用同一组 SPI / DC / CS / RST / BUSY 接线，详见 `README.md`；整机接线示意见 `hardware/开发板接线图.png`）:**
```
SPI (EPD):
  SCK  = GPIO4
  MOSI = GPIO5
  DC   = GPIO7
  CS   = GPIO15
  RST  = GPIO6
  BUSY = GPIO16

USB (CH340):
  TX   = GPIO43
  RX   = GPIO44

按键:
  BOOT = GPIO0
  RST  = GPIO18
  SW3  = GPIO9
  SW4  = GPIO46
  SW5  = GPIO3

电源管理:
  EN   = GPIO1
  ADC  = GPIO10
```

#### 墨水屏

**4.2" SSD1619:**
- 分辨率：400 × 300
- 颜色：黑白红三色
- 接口：SPI
- 刷新：~15 秒
- 功耗：~150mA (刷新)

**5.83" UC8179:**
- 分辨率：648 × 480
- 颜色：黑白红三色
- 接口：SPI
- 刷新：~20 秒
- 功耗：~200mA (刷新)

### 3.2 电源设计与充电路径

根据最新原理图（`SCH_Schematic2_2026-04-20.pdf`），本项目采用针对低功耗优化的单节锂电池充放电架构：

```text
       USB 5V (Type-C)
             │
             ▼
      [ TP4054 充电 IC ] ──(CHRG)──> LED1 (红灯)
             │
             ▼ (VBAT, 锂电池)
             │
      [ MSK12C02 拨动开关 ]
             │
             ▼ (VIN)
    [ HE9073A33M5R LDO ] ──> 3.3V (低静态系统电源)
             │
             ├─> ESP32-S3
             ├─> EPD 面板
             └─> 其它外设
```

**关键器件特性:**
- **锂电池管理**：使用 **TP4054-42-SOT25R** 线性充电芯片。配置 1kΩ (R8) 限制最大充电电流约 500mA，经 R6(2k) 驱动 LED1 作为充电指示。
- **稳压降压**：弃用漏电高的 AMS1117，改用 **HE9073A33M5R** (微安级静态电流)，这是支撑 Deep Sleep 数月续航的核心基础。
- **电量检测**：通过 100kΩ (R7) 和 100kΩ (R10) 对 VBAT 进行 1:2 分压（`BAT_DET`），由 ESP32-S3 内部 ADC 读取电压。

### 3.3 接口与物理器件

- **按键交互**：使用 3 颗侧按轻触开关（TS35CA），分别对应 GPIO3、GPIO46、GPIO9。常态拉高，按下接地（GND）。
- **物理接口**：使用 16-pin USB Type-C 母座（20009-UCAF001-X），CC1/CC2 连接下拉电阻以诱骗 5V 供电；屏幕连接采用 8-pin 墨水屏 FPC 接口。

### 3.4 PCB 物理参数

**板层:** 2 层
**尺寸:** 80×60mm
**工艺:**
- 线宽/线距：≥6mil
- 过孔：≥0.3mm
- 板厚：1.6mm
- 表面处理：有铅喷锡

**布局要点:**
1. ESP32 模组居中放置
2. 电源部分靠近输入端
3. EPD 接口靠近一侧
4. USB-C 靠近边缘
5. 按键均匀分布

---

## 四、软件架构

### 4.1 启动流程（双路径）

`app_main()` 在 `power_mgr_init()` 之后分支：

```
上电 / 复位 / 唤醒
        │
        ▼
   power_mgr_init()  （读深睡配置 + 唤醒原因）
        │
        ├── 深睡已启用 且 唤醒原因为 RTC 定时器 ──► quick_refresh_and_sleep()
        │        · STA 联网、SNTP、EPD、业务模块最小集
        │        · 刷新 NVS 记忆的显示模式后再次 deep sleep
        │        · 无 SoftAP、无 mDNS、无 http_app、无按键任务
        │
        └── 其他（上电、按键 GPIO3/9 唤醒等）──────► full_boot()
                 · AP+STA、mDNS、HTTP、按键、欢迎屏逻辑、power_mgr_arm()
```

完整逻辑见 `main/app_main.c`。

### 4.2 主要 FreeRTOS 任务（摘要）

| 组件 | 典型任务名 | 说明 |
|------|------------|------|
| HTTP | `httpd` | `esp_http_server` 处理请求；`open_fn` 可重置闲置计时 |
| 轮播 | `slideshow` | EventGroup 驱动，`scheduler.c` |
| 天气 | `weather` | 定时/事件拉取 HTTPS，`weather.c` |
| 时钟 | `clock_disp` | 表盘刷新，`clock_display.c` |
| 闲置 | `pwr_idle` | 深空前闲置检测，`power_mgr.c` |
| 按键 | `btn_poll` / `btn_disp` | 轮询 + 显示队列，`button.c` |

### 4.3 内存管理

**内部 SRAM**：动态堆约数百 KB 量级。N16R8 上 JPEG 解码 + 帧缓冲等大分配可由 8MB PSRAM 承接；仅在 N16（无 PSRAM）机型上需避免长时间叠加大分配，详见 `IMPLEMENTATION.md` 峰值表与优化手段。

**要点：** 双平面帧缓冲合并一次 `malloc`、`epd_display_fb_free` 早释放、PNG 路径内存高于 JPEG、画廊/转换时注意 SPIFFS 与堆竞争。

### 4.4 低功耗与功能的交集

| 场景 | HTTP | 轮播定时切图 | 天气后台刷新 | 备注 |
|------|------|--------------|--------------|------|
| `full_boot` 运行中 | 是 | 是 | 是 | 正常上电/复位/按键唤醒均保持 AP+STA、mDNS 和 HTTP；低功耗开关只影响睡眠与 WiFi PS 策略 |
| 定时器唤醒 quick 路径 | 否 | 否 | 按模式 | 仅刷当前模式一帧；本地页面跳过 WiFi，天气/额度/需要校时的页面启 STA-only |
| 按键唤醒 | 是（完整启动） | 是 | 是 | EXT1，非 timer wake |

**WiFi 策略边界**：低功耗按钮关闭时，运行期 WiFi 使用 `WIFI_PS_NONE`；低功耗按钮开启时，正常后台仍在线，运行期 WiFi 使用 `WIFI_PS_MIN_MODEM`。只有 `power_mgr_is_timer_wake()` 为真时才走 quick-refresh 并按模式决定是否启动 STA-only WiFi。

**OTA / 大文件上传**：闲置计时在 **新 TCP 会话建立** 时重置；极慢的传输若超过 `idle_timeout_s`，理论上可能触发深睡，刷机时可暂时关闭深睡或拉大闲置时间。

### 4.5 HTTP 安全（Basic Auth）

- **当前策略**：在 NVS 中写入用户名/密码后即视为启用鉴权；`http_check_basic_auth()` 已覆盖 OTA、上传、删除、配置保存、推屏等多数敏感 **POST**。**未配置凭据时通常不拦截**（便于首次配网）。
- **当前状态**：API、配置、数据和媒体类 **GET** 已统一纳入 Basic Auth，包括 `/status`、`/wifi_status`、`/scan`、`/images`、`/image`、`/weather_config`、`/msg_config`、`/canvas_layout`、`/timetable.json`、`/todo.json`、`/countdown_config` 等；页面 HTML 保持可打开，用于触发浏览器登录流程。
- **OTA 强制鉴权**：`POST /ota` 现在要求必须先在 `/auth_config` 设置凭据，未设凭据时直接返回 `403 Forbidden`，防止内网扫描到设备即可刷写固件。
- **天气**：`GET /weather_config` 不返回明文 API Key，且已受 Basic Auth 保护；`POST` 可通过空字符串跳过改 key、`api_key_clear` 等策略与固件版本一致。
- **前端**：`config.html` / `index.html` / `timetable.html` / `todo.html` / `countdown.html` / `board.html` 使用 `localStorage`（`epd_auth_u`、`epd_auth_p`）构造 Basic 头；**需先在配置页保存认证**，子页面才能以带鉴权方式通过校验。
- **备份恢复**：配置导出/导入刻意排除 `/auth_config`，不会备份或恢复 Basic Auth 密码；恢复配置后如需访问保护，必须在配置页重新设置管理账号。

### 4.6 并发与同步机制（v2.1.0 补丁）

为了保证多任务并发现场的系统稳定性，设计了多层数据保护机制：
- **`portMUX_TYPE` 自旋锁**：保护极短的临界区数据，例如 `power_mgr` 中的 64 位时间戳 `s_last_activity_us`（防止双核读取撕裂），以及 `scheduler`、`weather`、`clock`、`wifi_manager` 等全局配置项（读多写少且无需阻塞）。
- **`SemaphoreHandle_t` 互斥锁**：用于耗时较长的临界区保护，如 `s_epd_mutex` 保护面板硬件接口（`epd_set_panel` 与刷新），防止 HTTP 推屏与按键切屏发生冲突；`timetable` 和 `todo` 模块的 `s_cfg_mutex` 保护配置读写。
- **C11 `atomic_bool`**：`time_sync` 的 `s_synced`、`message_board` 的 `s_pending`、`display_policy` 的 `s_manual_screen` 使用 `<stdatomic.h>` 原子布尔，避免自旋锁开销。
- **数据副本解耦**：`weather` 模块引入 `weather_get_data_copy()` 提供数据副本，替代原有的全局裸指针访问，消除 `clock_display` 混合渲染天气摘要时可能遇到的读写数据竞争。

### 4.7 显示仲裁与异步语义

`display_policy` 负责统一显示所有权。用户主动触发的显示入口调用 `display_policy_begin_manual_display()` 后会同时设置手动画面、递增 display epoch 并通知轮播让出；各渲染任务在最终 `epd_display_*()` 前调用 `display_policy_epoch_is_current(epoch)`，发现已被新请求抢占时直接丢弃旧帧。

对仍采用任务渲染的接口，HTTP 返回语义已收紧：`/calendar_show`、`/canvas_show`、`/msg_show` 会等待本次任务结束再返回；如果期间出现更新的显示请求，JSON 中返回 `canceled=true`，调用方不应把这种结果当作最终上屏成功。后续如引入全局 display queue，应继续保留“排队、完成、取消”三种状态的明确区分。

EPD 初始化失败时，完整启动路径不再直接 `ESP_ERROR_CHECK()` 重启循环，而是尽量保留 AP/Web，供用户在网页或串口日志中诊断 BUSY 极性、屏幕排线和面板配置；定时快速刷新路径失败则记录日志并回到深睡，避免无人值守时反复重启耗电。

---

## 五、模块详细说明

### 5.1 EPD 驱动模块 (`epd_stub.c`)

**功能:**
- SSD1619 (4.2" 400×300 BWR) / UC8179 (5.83" 648×480 BWR) 驱动
- 已验证微雪 5.83" BWR B V2 (648×480 BWR) 驱动
- ATC/Solum 实验面板：SSD1619 1.3"/1.6"/2.2"/2.6"/2.9"、UC8151 2.9"、UC 4.3"、dual SSD 5.85"、UC8159 6.0"、UC8179 7.4"、SSD 9.7"
- EPD-nRF5 参考实验面板：SSD1619 4.2" BW/BWR、UC8176 4.2" BW/BWR、UC8179 7.5" BW/BWR、UC8159 5.83" BW、UC8159 7.5" low BWR、SSD1677 7.5" HD BW/BWR、JD79668 4.2" BWRY、JD79665 7.5" BWRY（图片转换生成独立黄色平面，JD796xx 按 2bpp 输出）
- JD79668/JD79665 黄色平面当前覆盖图片上传、画廊转换和测试图案；内置业务页面仍沿用黑/红两平面 framebuffer
- 除已验证微雪 5.83" BWR B V2 外，早期微雪 2.9"/2.66"/2.7"/4.26"/5.83 BW 等未验证入口已移除，旧 NVS 面板编号会回退默认 4.2"
- SPI DMA 批量传输
- BUSY 信号处理
- 全刷为当前稳定路径；快刷 / 局部刷新入口已从当前固件移除，优先保证三色屏显示稳定性

**关键函数:**
```c
esp_err_t epd_init(void);
esp_err_t epd_display_from_buffer(const fb_t *fb);          /* 全刷，BWR */
esp_err_t epd_display_fb_free(fb_t *fb);                    /* 同上，SPI 完成立即释放 fb */
esp_err_t epd_display_from_file(const char *path);
esp_err_t epd_repair_ghosting(int cycles, int pattern);     /* 残影修复：普通翻转 / 图案 / 强力组合 */
esp_err_t epd_set_panel(epd_panel_t panel);
epd_panel_t epd_get_panel(void);
```

**技术要点:**
1. DMA 4KB 分块传输（`EPD_FILL_CHUNK`），非 DMA-capable 源用 bounce buffer
2. BUSY 极性自动检测 + NVS `epd/busy_idle` 覆盖（4.2" 默认 0、5.83" 默认 1）
3. 多面板参数自动适配（`s_specs[]` 表）；`/panel_config` 当前只保存 NVS 并提示重启生效，避免热切换期间旧 framebuffer/raw 文件按新尺寸读取
4. ATC/Solum 移植参考 atc1441/Tag_FW_nRF52811 的控制器序列；EPD-nRF5 参考入口按参考工程的控制器命令模型重写接入；两类实验入口只保留 ESP32 当前硬件可表达的 SPI/BUSY/帧缓冲路径，UC8159 缺少源项目外部 EPD EEPROM/3 线回读脚位，使用默认 VCOM/PLL，需实机验证
5. 刷新策略：当前固件只暴露稳定全刷路径。4.2" SSD1619/SSD1683 与 5.83" UC8179 的局刷/快刷实验路径均不作为上线功能开放。
6. **局部刷新策略**：
   - 4.2" SSD1619/SSD1683 与 5.83" UC8179 都验证过局刷或快刷实验路径，但三色屏画面稳定性不达上线要求。
   - 当前固件不暴露快刷 / 局刷 HTTP 入口，也不提供局部刷新 API；时钟等自动页面统一走稳定全刷。
7. **残影管理**：配置页提供手动“刷屏修复”，支持普通全屏翻转、棋盘格/条纹图案和强力组合，轮数 1/2/3/5/8。

#### 5.1.1 时钟任务事件循环（`clock_task` in `clock_display.c`）

时钟模块当前统一走稳定全刷，事件循环主要决定何时需要刷新以及如何避免重复全刷。

**核心数据**：
- `s_state` 渲染状态镜像（`s_state_mux` 自旋锁保护），记录上次成功渲染时的
  `last_minute / last_hour / last_mday / last_weather_sig / last_show_weather /
  force_full_next`。
- 三个事件位：`BIT_CFG_CHANGED`（配置真正变化）、`BIT_WEATHER_DATA`（天气签名
  变化）、`BIT_WAKE`（外部 `clock_display_show` 渲染完后唤醒任务接管，但**不**
  触发再渲染）。

**单轮迭代决策**（聚合，最多一次 render）：

```
等待 (CFG | WEATHER | WAKE)，超时对齐到下一分钟边界 (1~60s)
   │
   ▼
读取 tm + show_weather + s_state 快照
   │
   ▼
聚合信号：minute_changed / hour_changed / day_changed
         cfg_changed / sw_visibility_changed / weather_data_changed
   │
   ▼
任意"涉及下半屏"信号 → force_full = true
仅 minute_changed → 按 `CLOCK_FULL_REFRESH_STEP_MIN` 判断是否到达全刷间隔
   │
   ▼
render_clock()（成功后内部更新 s_state）
```

**关键去重点**：
1. `clock_display_notify_weather_data` 入口先 `compute_weather_sig()` 与
   `s_state.last_weather_sig` 比较，相同直接 `return`，根本不 `setBits`。
2. `clock_display_set_config` 比对新旧 `enabled/style/show_weather`，无差别时不
   `setBits(CFG_CHANGED)`。
3. `clock_display_show` 同步渲染后只 `setBits(BIT_WAKE)` 而不 `BIT_CFG_CHANGED`，
   任务唤醒后通过 `s_state` 已是最新而跳过冗余刷屏。

**渲染日志**：每次决定渲染前打印
`tick HH:MM -> render clock refresh=full min=X full_step=X hr=X day=X cfg=X sw_vis=X wx=X ff=X`，
便于定位"为什么本分钟触发刷新"。

### 5.2 图片转换模块 (`image_convert.c`)

**功能:**
- JPEG/PNG/BMP解码
- Floyd-Steinberg 抖动
- HSV 红色识别
- 自适应缩放

**支持格式:**
```
JPEG: Baseline, Progressive
PNG:  8/16/24/32-bit
BMP:  24/32-bit RGB
```

**算法流程:**
```
输入图片
   │
   ▼
格式检测 (Magic Bytes)
   │
   ▼
解码 (TJpgDec/LodePNG/BMP)
   │
   ▼
RGBA → RGB 转换
   │
   ▼
HSV 红色识别
   │
   ▼
Floyd-Steinberg 抖动
   │
   ▼
Black/Red Plane 输出
```

### 5.3 WiFi 管理模块 (`wifi_manager.c`)

**功能:**
- AP/STA 共存（APSTA 模式，无需在连接 STA 时断开 AP 客户端）
- 配网管理（NVS 凭据持久化）
- WPA2/WPA3 (SAE) 自适应认证
- 指数退避自动重连（10s→300s）
- mDNS 服务

**状态机:**
```
INIT → AP_MODE → SCANNING → CONNECTING → CONNECTED
                      ↑                      │
                      └────── DISCONNECT ────┘
```

**重连策略:**
- 指数退避：10s → 300s
- 最大重试：10 次
- 失败回退：AP 模式

### 5.4 HTTP 服务器模块 (`http_app.c` + `http_gallery.c` + `http_features.c` + `http_canvas.c`)

**路由数量:** 73 条（`http_app_start` 中 `uris[]` 静态表，`sizeof(uris)/sizeof(uris[0])`）
**认证方式:** HTTP Basic Auth
**最大连接:** 5 个并发

**路由分组（共 73 条；以下按功能归类，完整注册表以 `main/http_app.c` 为准）：**

| 分组 | 路径示例 |
|------|-----------|
| 页面与状态 | `GET /` `/config` `/gallery` `/weather` `/clock` `/calendar` `/message` `/codex` `/favicon.ico` `/miaooaim-mark.png` `/status` |
| 画廊 | `POST /upload` · `GET /images` `/image` · `POST /show` `/delete_image` `/delete` |
| 网络与面板 | `GET /wifi_status` `/scan` · `POST /wifi_connect` `/wifi_forget` · `GET|POST /panel_config` |
| 轮播 / 认证 / 电源 | `GET|POST /slideshow` · `GET|POST /auth_config` · `GET|POST /power_config` |
| 画布留言板 | `GET /board` · `GET|POST /canvas_layout` · `POST /canvas_show` · 图标图片资源管理 |
| 天气 / 时钟 / 额度 / 留言 | `GET|POST /weather_config` · `POST /weather_show` · `GET|POST /clock_config` · `POST /clock_show` · `GET|POST /codex_quota_config` · `POST /codex_quota_show` · `GET|POST /msg_config` · `POST /msg_show` |
| 日历 / 课程表 / 待办 / 倒计时 | `POST /calendar_show` · `GET /timetable` `/timetable.json` · `POST /timetable` `/timetable_show` · `GET /todo` `/todo.json` · `POST /todo` `/todo_show` · `GET /countdown` · `GET|POST /countdown_config` · `POST /countdown_show` |
| 系统 | `GET /version` · `POST /epd_test` · `POST /spiffs_remount` `/spiffs_format` · `POST /ota` |

### 5.5 帧缓冲渲染模块 (`fb_render.c`)

**功能:**
- 像素绘制
- 几何图形
- UTF-8 文字渲染
- 位图绘制

**字库:**
- 紧凑内置字库：ASCII 95 字符 (8×16) + 中文 6814 字符 (16×16)，由 `tools/gen_font.py` 使用本地字体缓存 `tools/fonts/noto/LXGW975YuanSC-400W.ttf` 生成；该缓存由 `tools/fetch_open_fonts.py` 按需下载，不随仓库提交。
- 外置 `fontfs` 字库：16/24/32px MEF，存放在独立 `fontfs` 分区，来源同为 OFL 开源字体。
- 扩展：常用汉字 3000+

**渲染原语:**
```c
fb_pixel(fb, x, y, color);
fb_line(fb, x1, y1, x2, y2, color);
fb_rect(fb, x, y, w, h, color);
fb_fill_rect(fb, x, y, w, h, color);
fb_utf8_scaled(fb, x, y, text, color, scale);
fb_bitmap(fb, x, y, black, red, w, h);
```

### 5.6 电源管理模块 (`power_mgr.c`)

**功能:**
- 深度睡眠
- 定时器唤醒
- EXT1 GPIO 唤醒（ESP32-S3 仅 **RTC GPIO 0–21**；原理图 SW4=GPIO46 **不可**写入 EXT1 掩码，否则 `esp_sleep_enable_ext1_wakeup` 失败、**两侧键也会无法唤醒**）
- 闲置监控

**功耗模式:**
| 模式 | 电流 | 唤醒源 |
|------|------|--------|
| 活跃 | 150-300mA | 按键/HTTP |
| 闲置 | 50mA | 按键/定时器 |
| 深睡 | 需按硬件实测 | EXT1/RTC；受 LDO、充电 IC、USB 转串口、屏幕转接板和 GPIO 状态影响 |

**深睡优化**：进入深度睡眠前，固件依次执行以下步骤以确保最低漏电流：
1. 等待当前 EPD 刷新完成（最多 20 秒轮询 BUSY）
2. `nvs_flush_all()` 刷写所有 NVS 命名空间，防止配置丢失
3. `usb_serial_jtag_ll_phy_enable_pad(false)` 关闭 USB PHY 模拟前端，消除 USB 端口的额外漏电
4. `esp_deep_sleep_start()` 进入深睡

**唤醒后的软件路径**（与 `power_mgr_is_timer_wake()` 一致：仅 **定时器 + 深睡启用** 才走 quick）：

- **RTC 定时器唤醒** → `quick_refresh_and_sleep()` → 无 HTTP。
- **EXT1 按键（GPIO3/9）/ 上电复位** → `full_boot()` → 有 HTTP。

**模式记忆与状态一致性 (v2.1.0 关键修复)**：
为防止“定时唤醒时显示页面与实际推屏功能不符”（即旧版仅通过物理按键更新 `last_mode` 导致的状态错位），所有 HTTP 触发的具体功能显示（如 `timetable_show`、`weather_show` 等）成功后均会调用 `power_mgr_save_mode()`。定时器唤醒的 quick 路径将精确还原最后一次由 Web 或按键下发的模式。

### 5.7 画布留言板 (`canvas_board.c` & `http_canvas.c`)

**功能:**
- WYSIWYG 可视化布局编辑与渲染
- 文本、矩形、椭圆、直线、自由路径
- 内置 1-bit 图标库及用户自定义图标上传
- 画廊图片任意位置、任意比例缩放混合渲染

**渲染流:**
```
[Web UI 布局] -> JSON 数组 -> [HTTP POST /canvas_layout]
  -> canvas_board.c 解析 JSON
  -> 按 Z-Index(数组顺序) 调用 fb_render 原语
  -> (包含实时按比例缩小解析 spiffs/cimg/ 下的 1-bit 图片缓冲)
  -> EPD 刷新
```

---

## 六、API 接口文档

### 6.1 图片管理 API

#### POST /upload

**功能:** 上传图片

**请求:**
```
Content-Type: multipart/form-data

file: <image.jpeg>
```

**响应:**
```json
{
  "ok": true,
  "path": "/spiffs/image_001.jpeg"
}
```

#### GET /images

**功能:** 获取图片列表

**响应:**
```json
{
  "files": [
    {"name": "image_001.jpeg", "size": 52340},
    {"name": "image_002.png", "size": 48120}
  ],
  "total": 2,
  "free_space": 9487654
}
```

#### POST /show

**功能:** 将画廊中已有文件解码并刷新到墨水屏（需 Basic Auth，若已启用）。

**请求:** Query 参数 `name` — 与 `GET /images` 返回的文件名一致，例如：

```http
POST /show?name=img_20260101.jpg
```

成功时正文多为纯文本 `OK`（与前端约定一致）。

### 6.2 配置 API

#### GET /wifi_status

**功能:** 获取 WiFi 状态

**响应:**
```json
{
  "sta_connected": true,
  "ssid": "YourWiFi",
  "rssi": -45,
  "ip": "192.168.1.100",
  "ap_ssid": "ESP32_EPD_2E5C78"
}
```

#### POST /wifi_connect

**功能:** 连接 WiFi

**请求:**
```json
{
  "ssid": "YourWiFi",
  "password": "12345678"
}
```

### 6.3 业务 API

#### GET /timetable.json

**功能:** 获取课程表配置

**响应:**
```json
{
  "enabled": true,
  "period_count": 10,
  "show_days": 5,
  "semester_start": 1743465600,
  "periods": [
    {"start_hour": 8, "start_minute": 0, "end_hour": 8, "end_minute": 45},
    {"start_hour": 8, "start_minute": 55, "end_hour": 9, "end_minute": 40}
  ],
  "grid": [
    [
      {"name": "高等数学", "room": "A101", "weeks": "1-16"},
      null,
      {"name": "大学英语", "room": "B203", "weeks": "1,3,5,7-9"}
    ],
    [null, null, null]
  ]
}
```

**数据结构说明:**
- `period_count`: 节次数量（1-12）
- `show_days`: 显示天数（5=工作日，7=含周末）
- `semester_start`: 学期起始 Unix 时间戳
- `periods[]`: 每节课的时间定义
- `grid[day][period]`: 7天×12节课程网格
  - `name`: 课程名称（最多 39 字符）
  - `room`: 教室（最多 19 字符）
  - `weeks`: 上课周次（如 "1-5,7,9-12"）

#### POST /timetable

**功能:** 设置课程表

**请求:** 同上 (JSON)

#### POST /timetable_show

**功能:** 显示课程表（需 Basic Auth，若已启用）。

**请求体（可选）** JSON：`{"day":0..6}`，`0`=周一 … `6`=周日；空体或无字段则按「今天」策略由固件解析。

**响应:**
```json
{"ok": true}
```

#### GET /countdown_config

**功能:** 获取倒计时配置

**响应:**
```json
{
  "enabled": true,
  "count": 2,
  "items": [
    {"title": "期末考试", "year": 2026, "month": 7, "day": 1, "active": true},
    {"title": "暑假", "year": 2026, "month": 7, "day": 15, "active": true}
  ]
}
```

#### POST /countdown_config

**功能:** 设置倒计时配置（需 Basic Auth，若已启用）

**请求:** 与 GET 结构相同：`enabled`、`count`（可选，由 `items` 推导）、`items[]` 每项含 `title`、`year`、`month`、`day`、`active`

#### POST /countdown_show

**功能:** 显示倒计时

**响应:**
```json
{"ok": true}
```

#### 显示类接口返回语义

`/calendar_show`、`/canvas_show`、`/msg_show` 会等待当前渲染任务结束再返回。正常完成时返回类似 `{"ok":true,"canceled":false}`；如果本次显示在等待期间被更新的上传、按键或网页推屏请求抢占，则返回 `canceled=true`，前端应提示“已被新显示请求替换”，不要再保存为最后成功模式。
### 6.4 画布留言板 API

#### GET /canvas_layout
**功能:** 获取画布元素的 JSON 布局数组
**响应:**
```json
[
  {"type":"text","x":10,"y":20,"text":"Hello","scale":2,"color":0},
  {"type":"rect","x":50,"y":50,"w":100,"h":50,"fill":1,"sw":2,"color":1}
]
```

#### POST /canvas_layout
**功能:** 覆盖保存画布元素的 JSON 布局数组（需 Basic Auth）
**请求:** 接收数组结构，大小受 `CANVAS_LAYOUT_MAX_BYTES` (通常4KB) 限制

#### POST /canvas_show
**功能:** 根据当前布局生成帧缓冲并触发 EPD 刷新（需 Basic Auth）

#### GET /canvas_icons
**功能:** 获取可用图标清单
**响应:**
```json
{
  "builtin": ["wifi", "battery"],
  "user": ["myicon1", "myicon2"]
}
```

#### POST /canvas_icon_upload?name=xxx
**功能:** 上传自定义图标（需 Basic Auth）
**请求:** Raw Body 必须精确为 32 字节（对应 16×16 1-bit 位图）

#### POST /canvas_icon_delete?name=xxx
**功能:** 删除用户上传的自定义图标（需 Basic Auth）

---

## 七、Web 前端文档

### 7.1 页面结构

**认证联动：** 各页面使用 `epdAuthHeaders()` / `epdFetchOpts()`（或等价逻辑）在 `fetch` 时附加 Basic 凭据；凭据来自配置页写入的 `localStorage`。

#### index.html - 主页

**功能模块:**
1. 设备信息卡片
2. 图片上传区域
3. 画廊网格
4. 网络状态
5. 快捷操作

**技术栈:**
- HTML5 + CSS3
- 原生 JavaScript (无框架)
- Fetch API
- localStorage

#### config.html - 配置页

**配置项:**
1. WiFi 配置
2. 屏幕选择（主测 4.2" / 5.83" 与 ATC/Solum、EPD-nRF5 参考实验面板）
3. 轮播配置
4. 天气配置
5. 时钟配置
6. 留言板配置
7. 低功耗配置
8. HTTP 认证
9. 配置导出/导入
10. OTA 固件升级

#### timetable.html - 课程表页

**功能:**
1. 学期开始日期设置
2. 节次时间定义（自定义每节课时间）
3. 7天×N节课程网格编辑
4. 课程编辑（名称/教室/上课周次）
5. 导出/导入 JSON
6. 推送到屏幕

#### todo.html - 待办事项页

**功能:**
1. 待办列表管理（最多 16 条）
2. 优先级切换（普通/重要/紧急）
3. 完成状态勾选
4. 进度条显示
5. 推送到屏幕

#### countdown.html - 倒计时页

**功能:**
1. 倒计时条目管理（最多 3 条）
2. 目标日期设置
3. 推送到屏幕

#### board.html - 画布留言板编辑器

**功能:**
1. 基于 HTML Canvas 的 WYSIWYG 可视化编辑器
2. 双层 Canvas 架构（主画布 + 选中框叠加层）
3. 支持文本、矩形（含圆角）、椭圆、直线、自由路径、图标、图片共 7 种元素
4. 拖拽定位、控制点缩放、Z-Index 图层排序
5. 三色选择（黑/红/白）、填充/描边切换
6. 内置图标库 + 用户自定义图标上传
7. 1-bit 图片上传（Floyd-Steinberg 抖动转换）、任意位置/比例混合渲染
8. 从 `/panel_config` 动态读取屏幕分辨率，自适应多面板尺寸
9. 布局 JSON 持久化保存与推送到墨水屏

**技术栈:**
- HTML5 Canvas API（2D 绘图）
- 原生 JavaScript（无框架，单文件内嵌）
- Fetch API + Typed Array（流式二进制传输）
- Floyd-Steinberg 抖动算法（浏览器端 JS 实现）

### 7.2 主题系统

**三套主题:**
1. **Loopy** (粉色系)
2. **Kitty** (可爱系)
3. **Lotso** (草莓熊)

**实现方式:**
```css
[data-theme="loopy"] {
  --bg: #FFF0F5;
  --ac: #FF9BB3;
  --tx: #5A3050;
  /* ... */
}
```

**切换逻辑:**
```javascript
function setTheme(name) {
  document.documentElement.setAttribute('data-theme', name);
  localStorage.setItem('epd_theme', name);
}
```

### 7.3 响应式设计

**断点:**
```css
@media (max-width: 900px) { /* 平板 */ }
@media (max-width: 480px) { /* 手机 */ }
```

**适配策略:**
- 弹性布局 (Flexbox)
- 网格布局 (Grid)
- 相对单位 (rem, %)
- 媒体查询

---

## 八、构建与部署

### 8.1 开发环境

**系统要求:**
- Windows 10/11 (64-bit)
- 空闲磁盘：10GB
- 内存：≥8GB

**软件安装:**
1. ESP-IDF（**最低 v5.5.1**；已验证 5.5.x 更高版本通常可用）
2. Python 3.8+
3. CMake 3.16+
4. Git

### 8.2 编译步骤

```powershell
# 1.下载源码解压

cd msp

# 2. 设置目标芯片

idf.py set-target esp32s3

# 3. 配置项目 (可选)

idf.py menuconfig

# 4. 编译

idf.py build

# 5. 烧录

idf.py -p COM3 flash

# 6. 监控

idf.py -p COM3 monitor
```

### 8.3 分区表

```csv
# Name,      Type, SubType,  Offset,    Size

nvs,         data, nvs,      0x9000,    0xC000      # 48KB（容纳 PHY 校准 + 多个模块的配置 blob）
otadata,     data, ota,      0x15000,   0x2000      # 8KB
phy_init,    data, phy,      0x17000,   0x1000      # 4KB
factory,     app,  factory,  0x20000,   0x300000    # 3MB
ota_0,       app,  ota_0,    0x320000,  0x300000    # 3MB
ota_1,       app,  ota_1,    0x620000,  0x300000    # 3MB
coredump,    data, coredump, 0x920000,  0x20000     # 128KB
fontfs,      data, spiffs,   0x940000,  0x2E0000    # 2.875MB
spiffs,      data, spiffs,   0xC20000,  0x3E0000    # 3.875MB
```

设计原则：
- app 三槽均为 3MB。公开仓库不保留构建输出目录，具体 app 体积以发布前重新 `idf.py build` 的 `size` 输出为准。
- `coredump` 保存 panic / watchdog 崩溃转储，便于现场设备复盘；配置在 `sdkconfig.defaults` 中启用 `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`。
- `fontfs` 独立保存内置 MEF 字库，和用户图片/画布文件分离，避免字体资源挤占用户文件区。
- 用户 `spiffs` 保持在 `0xC20000`，减少分区调整时对已有用户数据的影响。
- 分区表或 `fontfs` 偏移变化后，普通 app OTA 不能完整迁移资源；发布/出厂应同时烧录 `partition_table.bin` 与 `fontfs.bin`，或使用完整 16MB 合并固件。

### 8.4 OTA 升级

**Web 界面升级:**
1. 打开 `/config` 页面
2. 选择固件文件 (.bin)
3. 点击"上传并升级"
4. 等待进度条完成
5. 设备自动重启

**命令行升级:**
```powershell
idf.py -p COM3 ota_flash
```

---

## 九、故障排查

### 9.1 常见问题

#### 问题 1: 上电无反应

**排查:**
1. 检查 USB 线
2. 测 USB 5V 输入
3. 测 3.3V 输出
4. 检查 LDO (HE9073A) 工作状态

#### 问题 2: 无法识别串口

**排查:**
1. 安装 CH340 驱动
2. 更换 USB 线
3. 检查 CH340 焊接
4. 测 USB D+/D-

#### 问题 3: WiFi 连接失败

**排查:**
1. 检查天线
2. 测 3.3V 电源
3. 查看串口日志
4. 检查 WiFi 配置

#### 问题 4: 墨水屏不显示

**排查:**
1. 检查排线方向
2. 重新插拔排线
3. 检查 FPC 焊接
4. 查看屏幕配置

### 9.2 日志级别

**ESP-IDF 日志:**
```
E (错误) - 严重错误
W (警告) - 潜在问题
I (信息) - 正常运行
D (调试) - 调试信息
V (详细) - 详细信息
```

**查看日志:**
```powershell
idf.py -p COM3 monitor
```

---

## 十、附录

### A. 文件清单

**源代码:**
- `main/app_main.c` - 主入口（双启动路径）
- `main/epd_stub.c` - EPD 驱动（SSD1619 / UC8179 / ATC-Solum / EPD-nRF5 参考面板）
- `main/http_app.c` - HTTP 服务器核心 + Basic Auth + OTA
- `main/http_gallery.c` - 画廊 HTTP 路由
- `main/http_features.c` - 功能模块 HTTP 路由
- `main/http_internal.h` - HTTP 子模块共享声明
- `main/wifi_manager.c` - WiFi 管理
- `main/image_convert.c` - 图片转换
- `main/fb_render.c` - 帧缓冲渲染
- `main/weather.c` - 天气模块
- `main/weather_icons_qw.c` - 和风天气离线 1-bit 图标库
- `main/clock_display.c` - 时钟显示
- `main/calendar_display.c` - 日历显示
- `main/lunar.c` - 农历算法
- `main/timetable.c` - 课程表（7×12 网格 + 学期周次）
- `main/todo.c` - 待办事项
- `main/countdown.c` - 倒计时
- `main/codex_quota.c` - Codex / 中转站额度看板
- `main/message_board.c` - 留言板
- `main/canvas_board.c` - WYSIWYG 画布留言板渲染器
- `main/http_canvas.c` - 画布 HTTP 路由与图标管理
- `main/button.c` - 按键驱动
- `main/battery_mon.c` - 电池电压检测与百分比估算
- `main/power_mgr.c` - 电源管理
- `main/scheduler.c` - 任务调度
- `main/display_mode.c` - 显示模式注册与切换
- `main/display_policy.c` - 显示策略协调
- `main/ui_theme.c` - 墨水屏页面通用 UI 主题组件
- `main/time_sync.c` - SNTP 时间同步
- `main/device_identity.c` - 设备标识
- `main/nvs_utils.c` - NVS 工具函数
- `main/spiffs_mount.c` - SPIFFS 挂载
- `main/lodepng.c` - LodePNG 第三方库

**第三方库与 ESP-IDF 组件:**
- `lodepng` — PNG 解码（仅解码器，已裁剪编码器/磁盘/辅助块/C++ 支持）
- `cJSON` — JSON 解析与生成（ESP-IDF 内置 `json` 组件，用于天气/课程表/待办/画布等模块）
- `esp_http_server` — HTTP 服务器
- `esp_http_client` + `esp-tls` — HTTPS 客户端（天气 API 请求）
- `app_update` — OTA 双分区升级
- `esp_driver_spi` — SPI DMA 驱动（EPD 通信）
- `spiffs` — 平铺文件系统（图片/图标/布局存储）

**Web 前端:**
- `web/index.html` - 主页
- `web/config.html` - 配置页
- `web/timetable.html` - 课程表
- `web/todo.html` - 待办事项
- `web/countdown.html` - 倒计时
- `web/board.html` - 画布留言板编辑器

**文档:**
- `README.md` - 项目介绍
- `文档索引.md` - 文档导航
- `ENGINEERING_DOCS.md` - 本文档
- `IMPLEMENTATION.md` - 实现说明
- `ROADMAP.md` - 当前维护路线图
- `COURSE_TABLE_README.md` - 课程表专题
- `hardware/BOM_ACTUAL.md` / `hardware/BOM.md` - 硬件与固件对齐提要（采购级 BOM 不公开）
- `https://oshwhub.com/team_voosogmo/project_fxbcjhaa` - 配套嘉立创开源硬件工程（4.2" SSD1619 PCB/外壳）

### B. 术语表

| 术语 | 英文 | 说明 |
|------|------|------|
| EPD | Electronic Paper Display | 电子纸显示器 |
| BWR | Black White Red | 黑白红三色 |
| PSRAM | Pseudo Static RAM | 伪静态 RAM |
| NVS | Non-Volatile Storage | 非易失性存储 |
| SPIFFS | SPI Flash File System | SPI 闪存文件系统 |
| OTA | Over-The-Air | 空中升级 |
| mDNS | Multicast DNS | 多播 DNS |
| STA | Station | 工作站模式 |
| AP | Access Point | 接入点模式 |
| DMA | Direct Memory Access | 直接内存访问 |

### C. 参考资料

1. [ESP32-S3 技术手册](https://www.espressif.com.cn/products/socs/esp32s3)
2. [ESP-IDF 编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.1/)

---

**文档版本:** v2.3.4-docs
**最后更新:** 2026-06-28
**维护者:** 项目维护者

---

*本文档随项目迭代持续更新；仓库以 Gitee 为准时请同步拉取最新提交。*
