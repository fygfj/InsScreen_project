# 对话存档：项目结构分析与硬件扩展评估

日期：2026-07-10  
工程路径：`D:\insScreen_project\InsScreen_project`  
主题范围：从“分析一下整个项目实现，以及结构”开始，到“蜂鸣器你注意到没有”结束。

## 1. 项目整体定位

当前工程是一个基于 ESP32-S3 的电子墨水屏信息终端固件。

主要能力包括：

- Web 配置页面
- 图片上传、图库管理、图片显示
- 天气显示
- 时钟显示
- 日历显示
- 课程表
- 待办事项
- 倒计时
- 留言板
- 画布编辑器
- Codex 额度页面
- OTA 升级
- 低功耗定时刷新
- SPIFFS 用户文件系统
- fontfs 外部字体分区

主程序入口：

```text
main/app_main.c
```

入口函数：

```c
void app_main(void)
```

## 2. 工程目录结构

当前主要目录含义：

```text
InsScreen_project/
├─ main/                固件主体 C 代码
├─ web/                 内嵌网页 UI，编译时打进固件
├─ spiffs_image/        构建 fontfs 分区用的外部字体资源
├─ tools/               字体生成、字体下载、辅助脚本
├─ docs/                文档资料
├─ hardware/            硬件资料
├─ release/             发布相关文件
├─ partitions.csv       分区表
├─ sdkconfig            ESP-IDF 配置
├─ sdkconfig.defaults   默认配置
├─ CMakeLists.txt       顶层构建逻辑
└─ main/CMakeLists.txt  主组件源文件、网页嵌入声明
```

## 3. 启动流程

`app_main()` 主要流程：

```text
NVS 初始化
↓
nvs_utils 初始化
↓
power_mgr 初始化
↓
battery_mon 初始化
↓
判断唤醒原因
↓
普通上电/按键唤醒 -> full_boot()
定时器唤醒       -> quick_refresh_and_sleep()
```

代码逻辑：

```c
if (power_mgr_is_timer_wake()) {
    quick_refresh_and_sleep();
} else {
    full_boot();
}
```

### 3.1 full_boot()

普通启动路径，设备会保持 AP/STA、HTTP、mDNS 后台可用。

主要动作：

- 挂载 `/spiffs`
- 初始化显示策略 `display_policy`
- 初始化外部字体 `font_ext`
- 初始化设备身份 `device_identity`
- 初始化 WiFi AP/STA
- 启动 mDNS
- 如果 STA 已连接，则启动时间同步
- 从 NVS 读取屏幕型号
- 预留 framebuffer 内存
- 启动 HTTP 服务
- 初始化 EPD 屏幕
- 初始化各业务模块
- 注册显示模式
- 恢复上次显示模式
- 初始化按键
- OTA 分区确认
- 启动低功耗空闲检测

### 3.2 quick_refresh_and_sleep()

低功耗定时唤醒路径。

主要目标是：

```text
最少初始化
↓
刷新当前显示模式
↓
重新进入深睡眠
```

它不会完整启动 AP/HTTP 后台，只有在当前显示内容确实需要网络时才短暂启用 WiFi，例如天气、Codex、需要联网时间的页面等。

## 4. 显示模式架构

显示模式注册在 `main/app_main.c` 的 `register_display_modes()`。

当前模式顺序：

```text
0 clock       时钟
1 calendar    日历
2 timetable   课程表
3 weather     天气
4 slideshow   轮播图
5 todo        待办事项
6 countdown   倒计时
7 codex       额度
```

核心文件：

```text
main/display_mode.c
main/display_mode.h
```

显示模式结构：

```c
typedef struct {
    const char           *name;
    const char           *label_cn;
    display_mode_show_fn  show;
} display_mode_entry_t;
```

显示请求入口：

```c
esp_err_t display_mode_show_request(int index, unsigned *epoch_out);
```

## 5. 显示仲裁与 epoch 机制

核心文件：

```text
main/display_policy.c
main/display_policy.h
```

它负责避免多个任务同时抢屏。

关键状态：

```text
manual_screen_active
quick_refresh_active
display_epoch
```

关键概念：

- 每次用户主动触发全屏显示，会递增 `display_epoch`
- 长时间渲染任务在真正刷屏前检查 epoch
- 如果 epoch 已经过期，说明用户已经触发了新显示，旧任务不能再覆盖屏幕

这套机制很重要，后续新增传感器页面、TF 卡页面、蜂鸣器提示时，都应该继续尊重这套策略。

## 6. 屏幕渲染流程

大多数页面渲染流程：

```text
fb_create()
↓
fb_clear()
↓
使用 fb_render 绘制文字/线条/图标/布局
↓
epd_display_fb_free()
↓
释放 framebuffer
```

核心文件：

```text
main/fb_render.c
main/fb_render.h
main/epd_stub.c
main/epd.h
```

EPD 当前硬件引脚：

```c
#define PIN_EPD_SCK   4
#define PIN_EPD_MOSI  5
#define PIN_EPD_DC    7
#define PIN_EPD_CS    15
#define PIN_EPD_RST   6
#define PIN_EPD_BUSY  16
```

EPD 使用：

```c
#define EPD_SPI_HOST SPI2_HOST
```

后续 TF 卡建议不要和 EPD 共用 SPI2，优先使用 SPI3。

## 7. Web/API 架构

网页文件目录：

```text
web/
```

网页通过 `main/CMakeLists.txt` 的 `EMBED_FILES` 嵌入固件。

HTTP 服务入口：

```text
main/http_app.c
```

HTTP 相关拆分：

```text
main/http_app.c       主 HTTP、WiFi、OTA、认证、配置、状态
main/http_gallery.c   图片上传、图库、显示、删除
main/http_features.c  课程表、待办、倒计时、日历显示
main/http_canvas.c    画布编辑器、图标、画布图片资源
```

主要页面：

```text
/             首页
/config       设置
/gallery      图库
/weather      天气
/clock        时钟
/calendar     日历
/message      留言
/codex        额度
/timetable    课程表
/todo         待办
/countdown    倒计时
/board        画布编辑器
```

## 8. 分区与文件系统

分区表：

```text
partitions.csv
```

主要分区：

```text
nvs        48KB       WiFi、天气、显示模式、屏幕配置等
factory    3MB        factory app
ota_0      3MB        OTA app 0
ota_1      3MB        OTA app 1
coredump   128KB      崩溃转储
fontfs     0x2E0000   外部字体 SPIFFS
spiffs     0x3E0000   用户图片、画布、上传资源
```

运行时文件：

```text
/spiffs/images/              图库图片
/spiffs/image.bin            当前转换后的屏幕 raw 图
/spiffs/upload.jpg           上传临时图
/spiffs/canvas_layout.json   画布布局
/spiffs/cimg/                画布图片
/spiffs/icons/               画布图标
/fontfs/fonts/cjk24.mef      24px 外部中文字库
/fontfs/fonts/cjk32.mef      32px 外部中文字库
```

SPIFFS 挂载文件：

```text
main/spiffs_mount.c
```

当前保护策略：

```c
.format_if_mount_failed = false
```

这是合理的。挂载失败时不自动格式化，避免用户数据被误删。

## 9. 外部字体系统

核心文件：

```text
main/font_ext.c
main/font_ext.h
```

外部字体分区：

```text
/fontfs
```

字体文件：

```text
/fontfs/fonts/cjk24.mef
/fontfs/fonts/cjk32.mef
```

构建时由顶层 `CMakeLists.txt` 调用：

```text
tools/fetch_open_fonts.py
tools/gen_ext_font.py
tools/gen_font.py
```

最终生成：

```text
build/fontfs.bin
```

## 10. NVS 配置分布

常见 namespace：

```text
wifi_cfg       WiFi 凭据
weather        天气配置和历史温度
clock          时钟配置
calendar       日历样式
timetable      课程表
todo           待办
countdown      倒计时
msgboard       留言板
slideshow      轮播图
power          低功耗和上次显示模式
epd            屏幕型号、busy_idle
http_auth      网页认证
codex_quota    Codex 额度页配置
```

## 11. 按键逻辑

核心文件：

```text
main/button.c
main/button.h
```

当前按键：

```text
GPIO9   上一个模式
GPIO46  刷新/切换日历样式
GPIO3   下一个模式
```

按键任务不直接刷屏，而是把命令发给 `display_worker_task`，避免小栈任务执行大渲染。

## 12. 低功耗策略

核心文件：

```text
main/power_mgr.c
main/power_mgr.h
```

主要能力：

- 空闲超时后进入深睡眠
- 定时唤醒刷新
- 保存/恢复当前显示模式
- 进入睡眠前等待 EPD 刷新稳定

深睡眠唤醒按键：

```text
GPIO9
GPIO3
```

注意：GPIO46 不能参与 ESP32-S3 的 EXT1 深睡眠唤醒，所以它只能运行时使用。

## 13. 当前工程维护注意点

维护重点：

1. 源码和文档中有部分中文注释显示为乱码，应该是历史编码问题，不影响当前编译，但影响阅读。
2. `.vscode/settings.json` 和 `.vscode/tasks.json` 当前被 Git 跟踪，并包含本机路径，提交前要决定是否保留。
3. `calendar_font_data.h` 和 `font_data.h` 很大，属于内置字体，继续加字体要注意固件体积。
4. 当前 `epaper_uploader.bin` 约 2.63MB，app 分区 3MB，剩余约 12%，继续加功能要关注分区空间。
5. 显示相关改动必须尊重 `display_policy` 的 epoch 机制。
6. SPIFFS/fontfs 不建议自动格式化。

## 14. 最新原理图硬件扩展评估

用户提供了最新原理图：

```text
c:\Users\18021\Desktop\六层版小电源\墨水屏工程\SCH_E-ink 板载_1_1-P1_2026-07-10.png
```

评估目标：

- 后续增加温湿度传感器
- 后续增加 TF 卡功能
- 注意蜂鸣器电路

## 15. GPIO 占用对照

当前固件已知引脚：

```text
EPD:
GPIO4  SCK
GPIO5  MOSI
GPIO6  RST
GPIO7  DC
GPIO15 CS
GPIO16 BUSY

USB:
GPIO19 USB-
GPIO20 USB+

按键:
GPIO9
GPIO46
GPIO3

电池检测:
GPIO10 BAT_DET

蜂鸣器:
GPIO17 BEEZER

AHT20:
GPIO1 SDA
GPIO2 SCL

TF 卡:
GPIO41 / GPIO42 / SD_MOSI_SD / SCK_SD / SDA_SD / SD_VDD_EN 等，需以最终网络名确认。
```

整体结论：

- AHT20 不和现有功能冲突。
- TF 卡可以做，但建议使用 SPI 模式。
- 蜂鸣器可以做，用 LEDC PWM 驱动。

## 16. 温湿度传感器 AHT20 评估

原理图中：

```text
AHT20_SCL -> GPIO2
AHT20_SDA -> GPIO1
VCC       -> 3V3
GND       -> GND
```

结论：可以做，风险低。

软件建议：

```text
main/aht20.c
main/aht20.h
```

使用 ESP-IDF I2C master。

AHT20 地址通常为：

```text
0x38
```

建议能力：

- 初始化检测
- 读取温度
- 读取湿度
- 状态缓存
- 读失败时保留上次有效数据
- Web 状态接口返回当前本地温湿度
- 时钟页/天气页可选择显示本地温湿度

硬件注意：

1. 如果是裸 AHT20 芯片，SCL/SDA 需要 4.7k 左右上拉到 3V3。
2. 如果是 AHT20 模块，很多模块自带上拉，避免重复过强上拉。
3. 传感器尽量远离 ESP32-S3、LDO、充电芯片、屏幕升压电路，否则温度偏高。

## 17. TF 卡功能评估

结论：可以做，建议先做普通 `/sdcard` 挂载，不建议一上来做 USB 读卡器模式。

推荐模式：

```text
SPI 模式优先
不优先 SDMMC 4-bit
```

原因：

- 项目已有 EPD、WiFi、HTTP、SPIFFS、fontfs、低功耗等复杂模块
- SPI 模式接入更稳
- 足够用于图片导入、配置备份、日志导出、字体包导入
- SDMMC 4-bit 速度更快，但硬件和软件复杂度更高

TF 卡 SPI 推荐映射：

```text
CS   -> DAT3/CD
MOSI -> CMD
MISO -> DAT0
SCK  -> CLK
```

DAT1/DAT2：

```text
SPI 模式下可以不接，或者保留上拉
```

当前 EPD 已使用：

```c
#define EPD_SPI_HOST SPI2_HOST
```

TF 卡建议：

```c
#define SD_SPI_HOST SPI3_HOST
```

这样 EPD 和 TF 卡不抢同一个 SPI host。

## 18. TF 卡供电评估

原理图中有 `SD_VDD` 电源开关，Q12 为 PMOS 高边开关。

从原理图看：

```text
R24 100k 拉到 3V3
SD_VDD_EN 通过 R25 控制 PMOS 栅极
```

推测逻辑：

```text
SD_VDD_EN = 0       -> 打开 TF 卡供电
SD_VDD_EN = 1/悬空 -> 关闭 TF 卡供电
```

软件中要明确写成：

```c
#define SD_CARD_POWER_ACTIVE_LOW 1
```

避免后续搞反。

硬件建议：

1. TF 卡座附近建议 `10uF + 100nF` 去耦。当前看到 C24 100nF，建议增加 10uF 或 22uF。
2. CMD/DAT0/DAT3 上拉到 SD_VDD 是对的，这样关闭 SD_VDD 后不会通过上拉反灌。
3. CLK 一般不需要强上拉，若图中 CLK 有 10k 上拉，建议至少预留可不焊。
4. 如果要热插拔，最好确认 CD 卡检测脚是否真实接到 MCU。

## 19. TF 卡软件接入建议

当前 sdkconfig 中 FatFS 已存在：

```text
CONFIG_FATFS_VOLUME_COUNT=2
```

后续可新增：

```text
main/sd_card.c
main/sd_card.h
```

`main/CMakeLists.txt` 需要补：

```cmake
fatfs
sdmmc
esp_driver_spi
esp_driver_gpio
```

建议接口：

```c
esp_err_t sd_card_init(void);
esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);
bool      sd_card_is_mounted(void);
esp_err_t sd_card_power_on(void);
esp_err_t sd_card_power_off(void);
esp_err_t sd_card_get_info(...);
```

推荐流程：

```text
sd_card_power_on()
↓
延时 20~50ms
↓
spi_bus_initialize(SPI3_HOST, ...)
↓
esp_vfs_fat_sdspi_mount("/sdcard", ...)
↓
使用文件
↓
fflush/fsync
↓
esp_vfs_fat_sdcard_unmount()
↓
sd_card_power_off()
```

## 20. TF 卡低功耗和断电保护

进入深睡眠前必须：

```text
停止 TF 卡读写
flush/fsync
unmount /sdcard
关闭 SD_VDD
```

唤醒后：

```text
需要用 TF 卡时再重新上电挂载
不需要就保持关闭
```

原因：

- 防止 FAT 文件系统损坏
- 避免 TF 卡在深睡眠中漏电
- 避免和之前 USB mode 类似的突然掉电损坏问题

建议先不要做：

```text
ESP32-S3 USB MSC 读卡器模式
```

这个会牵扯：

- TinyUSB
- USB console/USB Serial/JTAG 兼容
- 文件系统互斥
- 主机拔插保护
- 掉电保护
- Web/设备本地访问和电脑访问互斥

复杂度明显高于普通 TF 卡挂载。

## 21. TF 卡功能路线建议

建议分阶段：

第一阶段：

```text
/sdcard 挂载
容量状态
文件列表
手动挂载/卸载
```

第二阶段：

```text
从 TF 卡导入图片到 /spiffs/images
从 /spiffs 导出图片到 TF 卡
配置备份/恢复
日志导出
```

第三阶段：

```text
从 TF 卡导入字体包
从 TF 卡更新网页资源/离线资源
必要时再评估 USB MSC 读卡器模式
```

## 22. 蜂鸣器电路评估

原理图中已看到蜂鸣器电路，位置在右上中间。

主控连接：

```text
U5 GPIO17 -> BEEZER
```

电路大意：

```text
BEEZER -> R16 1k -> Q3 XR2318 控制端
Q3 + D4 + R17 + C23 驱动 BUZZER1
BUZZER1 标注 4kHz
```

结论：可以做。

如果蜂鸣器是无源蜂鸣片/压电片：

```text
使用 LEDC PWM
频率 4kHz
占空比 50%
```

如果蜂鸣器是有源蜂鸣器：

```text
GPIO 拉高/拉低即可
不需要 4kHz PWM
```

由于原理图标注 `4kHz`，更像按无源蜂鸣器设计，软件建议按 PWM 做。

## 23. 蜂鸣器软件接入建议

建议新增：

```text
main/beeper.c
main/beeper.h
```

宏控制：

```c
#define APP_ENABLE_BEEPER 1
```

推荐接口：

```c
esp_err_t beeper_init(void);
void      beeper_beep_ms(uint32_t ms);
void      beeper_pattern_success(void);
void      beeper_pattern_error(void);
void      beeper_pattern_click(void);
void      beeper_set_enabled(bool enabled);
bool      beeper_is_enabled(void);
```

推荐事件：

```text
按键反馈
WiFi 配网成功/失败
TF 卡挂载成功/失败
低电量提醒
倒计时结束提醒
上传/刷新完成提示
错误提示
```

注意：

- 不要在中断里直接长时间鸣叫。
- 使用短任务或 esp_timer 控制停止。
- 默认提示音要短，避免电子墨水屏设备显得烦。
- 加 NVS 配置，允许用户关闭声音。

## 24. 硬件扩展总体结论

这版原理图支持后续增加：

```text
AHT20 温湿度
TF 卡
蜂鸣器
```

推荐优先级：

```text
1. 蜂鸣器基础驱动
2. AHT20 温湿度采集
3. TF 卡 SPI 挂载
4. TF 卡导入/导出功能
5. 再考虑 USB MSC 读卡器模式
```

主要风险点：

```text
TF 卡供电电容是否足够
TF 卡 SPI/SDMMC 最终引脚映射是否明确
TF 卡掉电/睡眠前是否正确卸载
AHT20 是否受主控/LDO/充电芯片热源影响
蜂鸣器到底是无源还是有源
新增功能后 app 分区空间是否还够
```

整体判断：

```text
能做。温湿度和蜂鸣器风险低，TF 卡也能做，但必须重视供电、挂载/卸载、低功耗前保护和文件系统一致性。
```

