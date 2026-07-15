# AHT20 温湿度传感器接入流程

## 原理图结论

- 模块：AHT20
- 供电：3V3
- 接地：GND
- I2C 地址：0x38
- SDA：AHT20_SDA -> ESP32-S3 IO1
- SCL：AHT20_SCL -> ESP32-S3 IO2
- 模块已带 I2C 上拉，固件里不依赖内部弱上拉。

已知占用 GPIO：

- 墨水屏：GPIO4/5/6/7/15/16
- 蜂鸣器：GPIO17
- 按键：GPIO9/46/3
- 电池检测：GPIO10
- AHT20：GPIO1/2

结论：硬件可行，GPIO 无明显冲突。

## 实现目标

1. 新增 AHT20 驱动，不引入第三方库。
2. 新增本地传感器状态接口，网页端能看到模块是否在线、温度、湿度和最近错误。
3. 将 AHT20 数据作为“室内温湿度”，不直接覆盖网络天气的室外温度。
4. 低功耗友好：不常驻高频轮询，只按需读取并缓存。

## 推荐模块拆分

### i2c_bus

新增：

- `main/i2c_bus.h`
- `main/i2c_bus.c`

职责：

- 初始化 I2C master bus
- 固定配置 SDA=GPIO1、SCL=GPIO2
- 默认频率 100 kHz，稳定优先
- 提供 AHT20 设备句柄或通用 bus 访问接口

建议使用 ESP-IDF 新版 I2C master API，避免 legacy driver 后续维护问题。

### aht20

新增：

- `main/aht20.h`
- `main/aht20.c`

职责：

- 初始化/检测 AHT20
- 触发测量
- 等待 busy 释放
- 读取原始数据
- CRC 校验
- 换算温度和湿度
- 返回明确错误码

基本协议：

- 地址：0x38
- 触发测量命令：`0xAC 0x33 0x00`
- 等待时间：约 80 ms
- 温度换算：`temperature_c = raw_temp * 200 / 1048576 - 50`
- 湿度换算：`humidity_percent = raw_humidity * 100 / 1048576`

### sensor_local

建议新增：

- `main/sensor_local.h`
- `main/sensor_local.c`

职责：

- 管理 AHT20 初始化状态
- 缓存最近一次有效数据
- 控制读取频率，避免同一轮页面刷新重复读取
- 对外提供线程安全快照

建议缓存结构：

```c
typedef struct {
    bool present;
    bool valid;
    float temperature_c;
    float humidity_percent;
    int64_t updated_ms;
    esp_err_t last_error;
} sensor_local_data_t;
```

## HTTP 接口设计

### GET /sensor_status

用于网页状态展示，不强制触发硬件读取，只返回当前缓存和驱动状态。

建议响应：

```json
{
  "ok": true,
  "enabled": true,
  "present": true,
  "valid": true,
  "temperature_c": 26.4,
  "humidity_percent": 58.1,
  "age_ms": 3200,
  "sda_gpio": 1,
  "scl_gpio": 2,
  "i2c_addr": "0x38",
  "last_error": ""
}
```

异常时：

```json
{
  "ok": true,
  "enabled": true,
  "present": false,
  "valid": false,
  "temperature_c": null,
  "humidity_percent": null,
  "age_ms": -1,
  "sda_gpio": 1,
  "scl_gpio": 2,
  "i2c_addr": "0x38",
  "last_error": "ESP_ERR_TIMEOUT"
}
```

### POST /sensor_read

用于网页“立即读取/检测”按钮。

行为：

- 立即触发一次 AHT20 读取
- 更新缓存
- 返回与 `/sensor_status` 同格式 JSON

### GET/POST /sensor_config

首版可以只做简单开关：

```json
{
  "enabled": true,
  "show_on_clock": true,
  "show_on_weather": true,
  "show_on_calendar": false
}
```

保存到 NVS，命名空间建议：`sensor`。

## 网页端设计

设置配置页新增“本地温湿度”小节：

- 开关：启用本地温湿度
- 状态：已检测到 / 未检测到 / 读取失败
- 当前数据：温度、湿度、更新时间
- 引脚信息：SDA GPIO1 / SCL GPIO2
- 按钮：立即检测
- 显示位置开关：时钟页、天气页、日历页

不要把功能说明写成大段教程，保持和现有设置页风格一致。

## 显示集成建议

优先级：

1. 时钟页：显示“室内 26.4C / 58%”
2. 天气页：新增室内温湿度信息块，不替换室外天气
3. 日历页：可选显示在小天气区域旁边

显示文案建议统一使用“室内”，避免用户误以为这是室外天气。

## 低功耗策略

- 不开永久读取任务。
- 页面刷新前按需读取一次。
- 读取失败不阻塞刷屏，继续使用上一次有效缓存或显示未检测到。
- 深睡唤醒后重新初始化 I2C/AHT20。
- 如果后续要极致省电，再考虑给 AHT20 供电加开关；首版不做。

## 实施顺序

1. 添加 `i2c_bus`，配置 GPIO1/2，确认 I2C 初始化成功。
2. 添加 `aht20` 驱动，先只做检测和单次读取。
3. 添加 `sensor_local` 缓存层。
4. 在 `app_main` 正常启动路径初始化 sensor，不影响定时唤醒最小路径。
5. 添加 `/sensor_status` 和 `/sensor_read`。
6. 设置页新增“本地温湿度”状态区和立即检测按钮。
7. 添加 `/sensor_config` 和显示位置开关。
8. 接入时钟页/天气页显示。
9. 编译验证。
10. 烧录 app 分区。
11. 网页点击“立即检测”，确认能读到温湿度。
12. 断开 AHT20 或模拟失败，确认页面显示错误但设备不崩溃。

## 验证清单

- I2C 初始化成功。
- AHT20 地址 0x38 有响应。
- `/sensor_status` 返回 JSON。
- `/sensor_read` 能触发新读数。
- 温度、湿度数值合理。
- AHT20 不在线时返回明确错误。
- 网页状态显示正常。
- 时钟/天气页不会因为传感器失败而刷屏失败。
- 深睡唤醒后仍可读取。
- 不影响蜂鸣器、按键、墨水屏和电池检测。

## 不做的事

- 不引入 Arduino/AHTxx 第三方库。
- 不用高频后台轮询。
- 不把室内温湿度直接覆盖网络天气。
- 不在传感器失败时触发整机重启。
