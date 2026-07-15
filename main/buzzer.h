#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 蜂鸣器硬件参数：
 * - 原理图中 BEEZER 信号接到 ESP32-S3 的 GPIO17。
 * - 原理图标注蜂鸣器使用 4kHz，所以把 4000Hz 作为默认音调。
 * - 默认每次短响持续 100ms。
 */
#define BUZZER_GPIO_NUM              17
#define BUZZER_DEFAULT_FREQUENCY_HZ  4000U
#define BUZZER_DEFAULT_ON_TIME_MS     100U

/**
 * 初始化蜂鸣器驱动。
 *
 * 这个函数会把 GPIO17 配置成 LEDC PWM 输出，但初始化完成后保持低电平，
 * 所以只调用 buzzer_init() 不会发出声音。重复调用也不会重复占用硬件资源。
 */
esp_err_t buzzer_init(void);

/**
 * 以最大响度开始持续鸣叫。
 *
 * @param frequency_hz 音调频率，允许范围是 100～10000Hz；常用值为 4000Hz。
 * @return ESP_OK 表示成功；未初始化或参数不正确时会返回对应错误码。
 */
esp_err_t buzzer_start(uint32_t frequency_hz);

/**
 * 按指定响度开始持续鸣叫。
 *
 * volume_percent 填 1～100。这里的“100%响度”实际对应 PWM 的 50%占空比，
 * 因为无源蜂鸣器在 50%占空比附近最响；如果真的输出 100%占空比，就会变成
 * 不变化的直流电平，反而没有 4kHz 声音。响度填 0 等同于停止蜂鸣器。
 */
esp_err_t buzzer_start_with_volume(uint32_t frequency_hz,
                                   uint8_t volume_percent);

/**
 * 停止鸣叫，并把 GPIO17 拉低，保证原理图中的三极管/MOS 管关闭。
 *
 * @return ESP_OK 表示成功；如果还没初始化则返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t buzzer_stop(void);

/** 查询驱动是否已经初始化：true=已经初始化，false=尚未初始化。 */
bool buzzer_is_initialized(void);

/** 查询蜂鸣器当前是否正在持续输出声音。 */
bool buzzer_is_running(void);

/** 获取当前音调频率；蜂鸣器停止时返回 0。 */
uint32_t buzzer_get_frequency(void);

/**
 * 按“次数、间隔、响度”播放一组短响（非阻塞）。
 *
 * 每次使用默认 4kHz 响 100ms，gap_ms 是两次声音之间保持安静的时间。
 * 函数只负责启动后台任务，然后立即返回，不会卡住墨水屏刷新、网络或按键任务。
 * 同一时间只能播放一组短响；上一组没结束时再次调用会返回状态错误。
 *
 * @param times 响几次，允许 1～100 次。
 * @param gap_ms 两次声音之间的安静时间，允许 0～60000ms。
 * @param volume_percent 响度百分比，允许 1～100。
 *
 * 调用示例：buzzer_beep_pattern(3, 200, 80);
 * 表示响 3 次，每次之间停 200ms，响度为 80%。
 */
esp_err_t buzzer_beep_pattern(uint32_t times, uint32_t gap_ms,
                              uint8_t volume_percent);

/**
 * 高级短响接口：除次数、间隔、响度外，还能自定义频率和每次响多久。
 * 参数顺序为：频率、次数、单次鸣叫时间、静音间隔、响度。
 */
esp_err_t buzzer_beep_pattern_ex(uint32_t frequency_hz, uint32_t times,
                                 uint32_t on_time_ms, uint32_t gap_ms,
                                 uint8_t volume_percent);

/** 查询后台短响任务是否仍在运行。 */
bool buzzer_pattern_is_running(void);

#ifdef __cplusplus
}
#endif
