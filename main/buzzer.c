#include "buzzer.h"

#include <stdatomic.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 使用 ESP32-S3 的 LEDC 外设产生稳定 PWM，不需要 CPU 手动快速翻转 GPIO。 */
#define BUZZER_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_TIMER      LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_0
/* 10bit 表示占空比可在 0～1023 之间调整，足够控制蜂鸣器响度。 */
#define BUZZER_DUTY_RESOLUTION LEDC_TIMER_10_BIT
/* 无源蜂鸣器在 50%占空比附近声音最大，10bit 下对应数值 512。 */
#define BUZZER_DUTY_50_PERCENT (1U << (BUZZER_DUTY_RESOLUTION - 1U))

/* 限制参数范围，防止后续误传异常值导致 LEDC 配置失败或任务等待太久。 */
#define BUZZER_MIN_FREQUENCY_HZ 100U
#define BUZZER_MAX_FREQUENCY_HZ 10000U
#define BUZZER_PATTERN_MAX_TIMES 100U
#define BUZZER_PATTERN_MAX_MS    60000U
#define BUZZER_PATTERN_STACK     2048U
#define BUZZER_PATTERN_PRIORITY  3U

static const char *TAG = "buzzer";

/*
 * 这些状态可能同时被主任务和后台短响任务读取，所以使用原子变量，
 * 避免两个任务碰巧同时访问时读到不完整状态。
 */
static atomic_bool s_initialized;
static atomic_bool s_running;
static atomic_bool s_pattern_running;
static atomic_uint_fast32_t s_frequency_hz;

typedef struct {
    /* 一组短响任务所需要的全部参数。 */
    uint32_t frequency_hz;
    uint32_t times;
    uint32_t on_time_ms;
    uint32_t gap_ms;
    uint8_t volume_percent;
} buzzer_pattern_t;

static buzzer_pattern_t s_pattern;

esp_err_t buzzer_init(void)
{
    /* 已经初始化过就直接返回，防止重复配置同一个 LEDC 通道。 */
    if (atomic_load(&s_initialized))
        return ESP_OK;

    /* 第一步：配置 LEDC 定时器，定时器决定 PWM 的频率和精度。 */
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_LEDC_MODE,
        .duty_resolution = BUZZER_DUTY_RESOLUTION,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = BUZZER_DEFAULT_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 第二步：把 LEDC 通道连接到原理图中的 GPIO17，初始占空比为 0。 */
    const ledc_channel_config_t channel_cfg = {
        .gpio_num = BUZZER_GPIO_NUM,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * 原理图使用低端开关管驱动蜂鸣器，因此 GPIO17 拉低时开关管关闭，
     * 初始化结束后再主动停止一次，确保上电过程不会误响。
     */
    err = ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial stop failed: %s", esp_err_to_name(err));
        return err;
    }

    atomic_store(&s_initialized, true);
    atomic_store(&s_running, false);
    atomic_store(&s_pattern_running, false);
    atomic_store(&s_frequency_hz, 0);
    ESP_LOGI(TAG, "ready on GPIO%d (default %lu Hz)",
             BUZZER_GPIO_NUM, (unsigned long)BUZZER_DEFAULT_FREQUENCY_HZ);
    return ESP_OK;
}

esp_err_t buzzer_start(uint32_t frequency_hz)
{
    return buzzer_start_with_volume(frequency_hz, 100);
}

esp_err_t buzzer_start_with_volume(uint32_t frequency_hz,
                                   uint8_t volume_percent)
{
    /* 先检查初始化状态和用户参数，错误参数不会改动当前硬件输出。 */
    if (!atomic_load(&s_initialized))
        return ESP_ERR_INVALID_STATE;
    if (volume_percent > 100)
        return ESP_ERR_INVALID_ARG;
    if (volume_percent == 0)
        return buzzer_stop();
    if (frequency_hz < BUZZER_MIN_FREQUENCY_HZ ||
        frequency_hz > BUZZER_MAX_FREQUENCY_HZ) {
        return ESP_ERR_INVALID_ARG;
    }

    /* LEDC 会根据时钟计算最接近的可实现频率，并返回实际频率。 */
    uint32_t actual_hz = ledc_set_freq(BUZZER_LEDC_MODE,
                                       BUZZER_LEDC_TIMER,
                                       frequency_hz);
    if (actual_hz == 0) {
        ESP_LOGE(TAG, "cannot set frequency to %lu Hz",
                 (unsigned long)frequency_hz);
        return ESP_FAIL;
    }

    /* 把用户的 1～100% 响度线性换算到 1～50% PWM 占空比。 */
    uint32_t duty = (BUZZER_DUTY_50_PERCENT * volume_percent + 99U) / 100U;
    esp_err_t err = ledc_set_duty(BUZZER_LEDC_MODE,
                                  BUZZER_LEDC_CHANNEL, duty);
    if (err != ESP_OK)
        return err;

    err = ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    if (err != ESP_OK) {
        (void)ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
        return err;
    }

    atomic_store(&s_running, true);
    atomic_store(&s_frequency_hz, actual_hz);
    return ESP_OK;
}

esp_err_t buzzer_stop(void)
{
    if (!atomic_load(&s_initialized))
        return ESP_ERR_INVALID_STATE;

    esp_err_t err = ledc_stop(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    if (err == ESP_OK) {
        atomic_store(&s_running, false);
        atomic_store(&s_frequency_hz, 0);
    }
    return err;
}

bool buzzer_is_initialized(void)
{
    return atomic_load(&s_initialized);
}

bool buzzer_is_running(void)
{
    return atomic_load(&s_running);
}

uint32_t buzzer_get_frequency(void)
{
    return (uint32_t)atomic_load(&s_frequency_hz);
}

static void buzzer_pattern_task(void *arg)
{
    (void)arg;
    /* 复制一份参数，避免后台任务运行期间全局参数被意外改变。 */
    buzzer_pattern_t pattern = s_pattern;

    for (uint32_t i = 0; i < pattern.times; i++) {
        /* 开始本次鸣叫。 */
        esp_err_t err = buzzer_start_with_volume(pattern.frequency_hz,
                                                 pattern.volume_percent);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pattern start failed: %s", esp_err_to_name(err));
            break;
        }

        /* 等待指定的鸣叫时间，然后关闭输出。 */
        vTaskDelay(pdMS_TO_TICKS(pattern.on_time_ms));
        (void)buzzer_stop();

        /* 最后一次响完后无需再等待静音间隔，任务可以直接结束。 */
        if (i + 1U < pattern.times && pattern.gap_ms > 0)
            vTaskDelay(pdMS_TO_TICKS(pattern.gap_ms));
    }

    (void)buzzer_stop();
    atomic_store(&s_pattern_running, false);
    vTaskDelete(NULL);
}

esp_err_t buzzer_beep_pattern(uint32_t times, uint32_t gap_ms,
                              uint8_t volume_percent)
{
    return buzzer_beep_pattern_ex(BUZZER_DEFAULT_FREQUENCY_HZ, times,
                                  BUZZER_DEFAULT_ON_TIME_MS, gap_ms,
                                  volume_percent);
}

esp_err_t buzzer_beep_pattern_ex(uint32_t frequency_hz, uint32_t times,
                                 uint32_t on_time_ms, uint32_t gap_ms,
                                 uint8_t volume_percent)
{
    if (!atomic_load(&s_initialized))
        return ESP_ERR_INVALID_STATE;
    if (frequency_hz < BUZZER_MIN_FREQUENCY_HZ ||
        frequency_hz > BUZZER_MAX_FREQUENCY_HZ ||
        times == 0 || times > BUZZER_PATTERN_MAX_TIMES ||
        on_time_ms == 0 || on_time_ms > BUZZER_PATTERN_MAX_MS ||
        gap_ms > BUZZER_PATTERN_MAX_MS ||
        volume_percent == 0 || volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 用原子比较保证同一时间最多只有一个短响任务。 */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_pattern_running, &expected, true))
        return ESP_ERR_INVALID_STATE;

    s_pattern = (buzzer_pattern_t) {
        .frequency_hz = frequency_hz,
        .times = times,
        .on_time_ms = on_time_ms,
        .gap_ms = gap_ms,
        .volume_percent = volume_percent,
    };

    /* 创建独立小任务，因此调用本函数的业务代码不需要在这里等待。 */
    BaseType_t ok = xTaskCreate(buzzer_pattern_task, "buzzer_pattern",
                                BUZZER_PATTERN_STACK, NULL,
                                BUZZER_PATTERN_PRIORITY, NULL);
    if (ok != pdPASS) {
        atomic_store(&s_pattern_running, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool buzzer_pattern_is_running(void)
{
    return atomic_load(&s_pattern_running);
}
