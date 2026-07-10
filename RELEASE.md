# 发布流程

本文面向维护者，说明如何把当前源码发布成可供 Gitee 用户下载的 16MB 整包固件，并保持 README、release 文档、校验值和 `firmware-download` 分支一致。

## 发布前确认

1. 当前分支为 `master`，且工作区只包含本次准备发布的源码、Web、文档或 release 改动。
2. `CMakeLists.txt` 中 `PROJECT_VER` 已更新到目标版本。
3. 如果修改了分区表、fontfs 字库、Web 嵌入资源或 bootloader 配置，本次必须重新生成整包固件。
4. 不提交本地生成目录：`build/`、`managed_components/`、`sdkconfig`、`spiffs_image/`、`tools/fonts/`、缓存目录和本地参考工程。

## 构建固件

在 ESP-IDF v5.5.x 环境中执行：

```powershell
idf.py fullclean
idf.py set-target esp32s3
idf.py build
```

构建完成后确认：

- app 没有超过 3MB app 分区。
- `build/fontfs.bin` 已生成。
- 构建输出中 target 为 `esp32s3`。

## 合并 16MB 整包

当前 release 整包面向 16MB Flash，合并命令：

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

说明：

- `0x920000` 是 `coredump` 分区，不需要合并单独镜像。
- `fontfs.bin` 的地址是 `0x940000`。
- 用户 SPIFFS 在 `0xC20000`，整包不主动写入用户图片区。

## 生成 ZIP 与校验

重新生成静态 ZIP：

```powershell
Compress-Archive -Path release\epaper_uploader_full_16MB.bin `
  -DestinationPath release\epaper_uploader_full_16MB.zip -Force
```

计算校验：

```powershell
Get-FileHash -Algorithm SHA256 release\epaper_uploader_full_16MB.bin
Get-FileHash -Algorithm SHA256 release\epaper_uploader_full_16MB.zip
Get-Item release\epaper_uploader_full_16MB.bin, release\epaper_uploader_full_16MB.zip |
  Select-Object Name,Length
```

同步更新：

- `release/README.md`
- `公开发布检查清单.md`
- `v2.3.3优化改动说明.md` 或对应版本说明
- `CHANGELOG.md`
- `README.md` 中如有版本、校验或下载说明变化

## 同步 firmware-download 分支

`firmware-download` 分支用于 Gitee 打包下载入口：

```text
https://gitee.com/gxp666111/miaomiao/repository/archive/firmware-download.zip
```

该分支只应保留：

- `README.md`
- `epaper_uploader_full_16MB.bin`

同步时应确认 `firmware-download:epaper_uploader_full_16MB.bin` 与 `master:release/epaper_uploader_full_16MB.bin` 为同一版本。不要把源码、build 目录、sdkconfig、font cache 或本地素材放到该分支。

## 发布前检查

至少运行：

```powershell
python tools/check_public_docs.py
git diff --check
git status --short
```

推荐实机抽检：

- 4.2" 主测屏：烧录、配网、图片上传、时钟、日历、天气、待办、倒计时。
- 5.83" 主测屏：天气、时钟、日历、待办、倒计时、欢迎页。
- OTA：成功升级、失败处理和回滚路径。
- 低功耗：正常开机保留 AP+STA/HTTP，RTC 定时唤醒按模式决定是否联网。
- SPIFFS：不 erase 时保留用户图片/配置；需要恢复时能通过 Web 页面处理。

## 推送顺序

1. 在 `master` 提交源码、文档和 `release/` 产物。
2. 推送 `origin/master`。
3. 更新并推送 `firmware-download` 分支。
4. 再次检查 `git ls-remote origin refs/heads/master` 与本地 HEAD 一致。
5. 如需更新 Gitee 右侧简介，使用 `tools/update_gitee_about.ps1` 或手动复制 `docs/gitee-about.md` 中的描述。

## 回滚原则

- 如果 release 校验、下载入口或分区地址有误，优先发布新的修正提交，不改写已推送历史。
- 如果固件存在会导致设备无法启动或用户数据丢失的风险，应在 README、release 说明和 Gitee 首页明确提示暂停下载，并尽快重新生成整包。
