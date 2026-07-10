# 第三方声明

本仓库主体代码以 `LICENSE` 中的 PolyForm Noncommercial License 1.0.0 发布。仓库中同时包含或使用若干第三方组件、字体和工具链资源，其授权条款以各自原始许可证为准。本文件用于帮助复刻用户和二次开发者快速识别这些外部来源。

再分发源码、修改版本或固件时，应保留 `LICENSE` 顶部的 `Required Notice`、Web 管理端页面底部的项目署名和 Gitee 源码仓库链接。若发布修改版，应清楚标注修改来源和修改内容，不得误导用户认为其为官方原版。

## ESP-IDF 与组件

| 项目 | 用途 | 来源 / 说明 |
|------|------|-------------|
| ESP-IDF | ESP32-S3 构建、驱动、网络、HTTP、NVS、SPIFFS、OTA 等基础能力 | 由 Espressif 提供，项目要求 ESP-IDF v5.5.x |
| `espressif/mdns` | 局域网 mDNS 发现 | 通过 `main/idf_component.yml` 声明，由 ESP-IDF Component Manager 获取 |
| `esp_http_server`、`esp_wifi`、`esp_timer`、`nvs_flash`、`esp_https_ota` 等 | 固件运行时基础组件 | ESP-IDF 随附组件 |

## 第三方源码

| 文件 | 用途 | 授权说明 |
|------|------|----------|
| `main/lodepng.c` / `main/lodepng.h` | PNG 解码 | LodePNG，原文件头部保留版权和许可声明 |
| TJpgDec / JPEG 解码路径 | JPEG 解码 | 通过 ESP-IDF / 组件依赖提供；具体版本和授权以构建环境中的组件为准 |

如后续新增第三方 C/C++ 源码，应保留原始版权头，并在本文件补充来源、用途和授权说明。

## 字体资源

公开固件字库由可再分发的开源字体生成，详见 `docs/fonts.md`。

| 字体 | 用途 | 授权 |
|------|------|------|
| `LXGW975YuanSC-400W.ttf` | 中文、英文、数字和常用符号主字形 | SIL Open Font License 1.1 |
| `NotoSansMono-Regular.ttf` | 部分符号/等宽字符兜底 | SIL Open Font License 1.1 |
| GNU Unifont 16.0.02 | 4.2 寸欢迎页和日历页专用 1bit 小字号位图 | GNU Unifont License / GPLv2+ with font embedding exception |

仓库不直接提交大体积 TTF 缓存文件；构建脚本会按需下载到 ignored 目录 `tools/fonts/noto/`，再生成 `main/font_data.h` 和 `fontfs` 使用的 MEF 位图字库。
`tools/unifont.hex.gz` 用于生成 `main/calendar_font_data.h` 的小范围字形表，避免 4.2 寸欢迎页、普通日历和带天气日历在小字号下出现 TTF 抗锯齿转 1bit 后发虚。

## Web 端字体渲染

留言/画板页面可以让浏览器使用用户本机字体渲染成图片后上传到设备。该路径属于用户运行时渲染结果，不等同于本仓库随固件分发对应字体文件。若要把新字体随固件或 release 分发，必须确认该字体允许再分发，并同步更新 `docs/fonts.md`、本文件和 release 说明。

## 图片、硬件素材与截图

| 路径 | 用途 | 说明 |
|------|------|------|
| `docs/images/` | README 展示图、硬件示意图、Web 截图 | 随公开仓库用于文档展示 |
| `hardware/` | 硬件兼容提要和接线示意 | 用于复刻说明，具体 PCB/外壳以嘉立创开源硬件工程为准 |
| `release/` | 免编译烧录包 | 由本仓库当前源码、分区表和 fontfs 构建生成 |

本仓库 `.gitignore` 排除了本地原始素材目录、外部参考工程、构建产物和字体缓存。公开文档使用的素材应优先放入 `docs/images/` 并保持文件名稳定。

## 维护要求

新增或替换第三方资源时，请同步检查：

- 是否允许公开分发、修改、嵌入固件或用于文档展示。
- 是否需要保留原始版权头、许可证文件或 NOTICE。
- 是否会影响 `release/` 固件、`fontfs`、`spiffs` 或下载包校验。
- 是否需要更新 `docs/fonts.md`、`RELEASE.md`、`公开发布检查清单.md` 和 `tools/check_public_docs.py`。
