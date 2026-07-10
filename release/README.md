# 喵哎-MiaooAim 免编译固件烧录指南

本文档面向不准备自行编译的复刻用户。只需要下载整包固件文件，并使用 Espressif Flash Download Tool 写入 ESP32-S3，即可完成基础烧录。

## 项目入口

```text
Gitee 固件仓库:       https://gitee.com/gxp666111/miaomiao
嘉立创开源硬件工程:  https://oshwhub.com/team_voosogmo/project_fxbcjhaa
```

## 固件文件

> **发布状态说明（2026-06-28）**：本目录已按 v2.3.4 当前源码重新构建并合并 `epaper_uploader_full_16MB.bin`。若源码、分区表或 fontfs 字库在此后继续变化，发布前必须在 ESP-IDF 终端重新 `idf.py build` 并重新合并整包。

推荐下载入口：

```text
https://gitee.com/gxp666111/miaomiao/repository/archive/firmware-download.zip
```

为了避免 Gitee raw 单文件下载把文件名保存成带单引号的形式，建议从主页的“下载固件烧录包”入口下载，或打开这个专用分支打包下载。Gitee 下载到本地的文件通常叫：

```text
firmware-download.zip
```

解压后使用整包固件：

```text
epaper_uploader_full_16MB.bin
```

ZIP 内只包含上述整包固件文件；解压后的 bin 校验信息以本文件为准。

校验信息：

```text
Size:   12713984 bytes
SHA256: A7EB0494BAD50D5D297A9AD67F24CE224664E9F05A0189D9A73F11A3210301EC
```

说明：推荐入口中的 `firmware-download.zip` 是 Gitee 按 `firmware-download` 分支动态打包生成的下载文件，不建议固定校验 ZIP 本身；请以解压后的 `epaper_uploader_full_16MB.bin` SHA256 为准。

仓库内同时保留 `release/epaper_uploader_full_16MB.zip`，它是随当前源码发布的静态 ZIP 产物，校验信息如下：

```text
Size:   2208934 bytes
SHA256: 749C81BAD63712F403AF4616DCDD9671143F9C426CBE8CAB4BFD02E848D8D664
```

同时保留带版本后缀的归档文件，便于人工核对具体发布版本：

如果手动点 raw 下载后文件名带了单引号，例如：

```text
'epaper_uploader_full_16MB.bin'
epaper_uploader_full_16MB.bin'
```

请手动重命名为：

```text
epaper_uploader_full_16MB.bin
```

这不是文件损坏，只是 Gitee raw 下载响应头导致的文件名问题。


适用范围：

```text
Chip:       ESP32-S3
Flash:      16MB
SPI Mode:   DIO
SPI Speed:  80MHz
App offset: 0x20000
```

## 下载工具一行烧录

在 Espressif Flash Download Tool 的 `SPIDownload` 页面只填一行：

```text
File:    release\epaper_uploader_full_16MB.bin
Address: 0x0000
```

推荐参数：

```text
SPI SPEED: 80MHz
SPI MODE:  DIO
DoNotChgBin: 勾选
BAUD: 115200 或 460800
COM: 选择设备对应串口
```

如果工具里有 Flash Size 选项，请选择：

```text
16MB
```

然后点击：

```text
START
```

等待下载完成后，设备会自动重启。

## 是否需要点 ERASE

新板子、配置混乱、SPIFFS 挂载异常时：

```text
可以先点 ERASE，再点 START
```

已经配置过 WiFi、天气、日历，不想丢配置时：

```text
不要点 ERASE，直接点 START
```

说明：

```text
整包固件会写入 bootloader、partition table、otadata、app 和 fontfs 字库分区。
不点 ERASE 时，NVS 配置区和 SPIFFS 数据区不会被主动清空。
```

## 整包内包含的分区

这个整包固件已经合并以下文件：

```text
0x0000   bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x15000  ota_data_initial.bin
0x20000  epaper_uploader.bin
0x920000 coredump (128KB，崩溃日志分区，用户不用手动选择)
0x940000 fontfs.bin
```

因此普通用户不需要再分别选择这些文件和分区。

## 常见问题

### 下载失败或一直等待

请检查：

```text
1. COM 口是否选对
2. USB 数据线是否支持数据传输
3. 串口是否被 IDF monitor、Arduino、其他下载工具占用
4. 必要时按住 BOOT 键，再点 START，开始后松开 BOOT
```

### 烧录后还是旧配置

如果希望清空旧 WiFi、天气、面板等配置：

```text
先点 ERASE，再点 START
```

### 烧录后无法访问设备

如果没有保存 WiFi 配置，设备会开启 AP：

```text
SSID: ESP32_EPD_xxxxxx
Web:  http://192.168.4.1/
```

连接 AP 后进入网页配置 WiFi、天气、Codex 额度和屏幕参数。

## 开发者重新生成整包

在 ESP-IDF 环境中先编译：

```powershell
idf.py build
```

然后合并固件：

```powershell
python -m esptool --chip esp32s3 merge_bin `
  -o release\epaper_uploader_full_16MB.bin `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x15000 build\ota_data_initial.bin `
  0x20000 build\epaper_uploader.bin `
  0x940000 build\fontfs.bin
```
