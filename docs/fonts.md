# 字库说明

当前固件有三条文字显示路径：

- `main/font_data.h`：紧凑内置字库，ASCII 95 个 8x16 字符 + 中文 6814 个 16x16 字符，用于小字号、兜底和启动期文本。
- `main/calendar_font_data.h`：16px 清晰位图表，覆盖 ASCII 和常用 CJK 基本区，供 4.2 寸密集 UI、欢迎页、日历、待办/倒计时等小字号文本使用，用于改善 1bit 小字号发虚问题。
- `fontfs` 分区：构建时生成的 24/32px MEF 位图字库，用于 4.2/5.83 寸主要 UI 文本、标题、正文和较大的普通数字。
- Web 画板/留言：浏览器把用户选择的字体渲染为图片后上传到设备，适合测试宋体、仿宋、书法类等本机或浏览器可用字体；该路径不要求把所有字体内置到固件。

## 字体来源

公开固件字库由 OFL 开源字体生成：

- `LXGW975YuanSC-400W.ttf`：中文、英文、数字和常用符号主字形。
- `NotoSansMono-Regular.ttf`：部分符号/等宽字符兜底。
- GNU Unifont 16.0.02：用于生成 `main/calendar_font_data.h` 的 16px 清晰小字位图。

这些字体遵循 SIL Open Font License 1.1。公开仓库保留下载和生成脚本，不直接提交大体积 TTF 缓存文件：`tools/fetch_open_fonts.py` 会在构建时把字体和许可文件下载到本地 ignored 目录 `tools/fonts/noto/`，随后 `tools/gen_font.py`、`tools/gen_ext_font.py` 生成固件使用的位图字库。
`tools/gen_calendar_font.py` 使用仓库内的 `tools/unifont.hex.gz` 生成 16px 清晰位图表，不占用 `fontfs` 或用户 SPIFFS 空间。

## 分区与生成

`partitions.csv` 中 `fontfs` 位于 `0x940000`，大小 `0x2E0000`（2.875MB）。构建时会先生成本地 ignored 输出 `spiffs_image/fonts/*.mef`，再由 ESP-IDF 生成 `fontfs.bin`，并随完整 16MB 固件一起烧录。

当前保留两档 MEF：

- `cjk24.mef`：正文、列表、紧凑页面内容。
- `cjk32.mef`：标题、较大普通数字和宽松页面内容。

16px 小字不再放入 `fontfs` 外置字库，改走固件内置清晰位图表，减少 1bit 小字号由外置圆体栅格化造成的发虚、歪斜和风格割裂，同时释放约 250KB fontfs 空间。

48px 或更大的请求默认复用 32px 字库做受控缩放，避免额外 48px 字库占满 flash。

`fontfs` 与用户 `spiffs` 是两个独立分区：fontfs 只放固件随附字库，用户 SPIFFS 只放图片、画布素材和运行时文件。配置页/上传页展示的 SPIFFS 进度条代表用户文件区使用率，不包含 fontfs。

## 发布注意

不要用 Windows 系统字体（微软雅黑、黑体、宋体、等线等）生成公开固件资源。网页留言/画板可以让浏览器临时渲染用户本机字体成图片，但固件内置资源必须来自可再分发字体。若以后替换字体，应同时更新本文件、生成脚本说明和 release 合并说明。

## 下载失败处理

首次构建会运行 `tools/fetch_open_fonts.py` 下载开源字体。如果国内网络访问 GitHub 原始文件超时，脚本会优先尝试 jsDelivr 镜像，也支持本地缓存：

```powershell
$env:MIAOO_FONT_SOURCE_DIR="D:\fonts\miaoo-open-fonts"
idf.py build
```

该目录需要包含 `LXGW975YuanSC-400W.ttf` 和 `NotoSansMono-Regular.ttf`。也可以直接把这两个开源字体复制到 `tools/fonts/noto/` 后重新执行 `idf.py build`。
