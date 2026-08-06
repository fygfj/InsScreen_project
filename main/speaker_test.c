#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "speaker_test.h"

static const char *TAG = "speaker_test";

#define SPK_BCLK_GPIO      GPIO_NUM_39
#define SPK_LRCLK_GPIO     GPIO_NUM_40
#define SPK_DIN_GPIO       GPIO_NUM_41
#define SPK_SD_MODE_GPIO   GPIO_NUM_42

#define SPK_SAMPLE_RATE_HZ       16000U
#define SPK_TONE_HZ              1000U
#define SPK_BEEP_DURATION_MS     120U
#define SPK_BEEP_GAP_MS          100U
#define SPK_BEEP_COUNT           2U
#define SPK_FRAMES_PER_CHUNK     256U
#define SPK_SD_MODE_SETTLE_MS    20U

static const int16_t s_sine_1khz_16k[16] = {
    0, 4592, 8485, 11087, 12000, 11087, 8485, 4592,
    0, -4592, -8485, -11087, -12000, -11087, -8485, -4592,
};

static uint8_t speaker_test_clamp_volume(uint8_t volume_percent)
{
    if (volume_percent < 1)
        return 1;
    if (volume_percent > 100)
        return 100;
    return volume_percent;
}

static esp_err_t speaker_test_set_amp(bool enabled)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SPK_SD_MODE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK)
        return err;

    return gpio_set_level(SPK_SD_MODE_GPIO, enabled ? 1 : 0);
}

static void speaker_test_fill_tone(int16_t *buffer, size_t frames,
                                   uint32_t *phase, uint8_t volume_percent)
{
    for (size_t i = 0; i < frames; ++i)
    {
        int32_t scaled = ((int32_t)s_sine_1khz_16k[*phase & 0x0FU] *
                          (int32_t)volume_percent) / 100;
        int16_t sample = (int16_t)scaled;
        buffer[i * 2] = sample;
        buffer[i * 2 + 1] = sample;
        *phase = (*phase + 1U) & 0x0FU;
    }
}

esp_err_t speaker_test_play_tone(uint8_t volume_percent)
{
    volume_percent = speaker_test_clamp_volume(volume_percent);
    ESP_LOGI(TAG, "Enable amp GPIO%d, play %u Hz I2S test at %u%% on BCLK=%d LRCLK=%d DIN=%d",
             SPK_SD_MODE_GPIO, SPK_TONE_HZ, (unsigned)volume_percent,
             SPK_BCLK_GPIO, SPK_LRCLK_GPIO, SPK_DIN_GPIO);

    esp_err_t err = speaker_test_set_amp(true);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Amp enable failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(SPK_SD_MODE_SETTLE_MS));

    i2s_chan_handle_t tx_chan = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = SPK_FRAMES_PER_CHUNK;
    chan_cfg.auto_clear = true;

    err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "I2S channel alloc failed: %s", esp_err_to_name(err));
        (void)speaker_test_set_amp(false);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws = SPK_LRCLK_GPIO,
            .dout = SPK_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err == ESP_OK)
        err = i2s_channel_enable(tx_chan);

    static int16_t samples[SPK_FRAMES_PER_CHUNK * 2];
    if (err == ESP_OK)
    {
        uint32_t phase = 0;
        const size_t beep_frames = (SPK_SAMPLE_RATE_HZ * SPK_BEEP_DURATION_MS) / 1000U;
        const size_t gap_frames = (SPK_SAMPLE_RATE_HZ * SPK_BEEP_GAP_MS) / 1000U;

        for (uint32_t beep = 0; beep < SPK_BEEP_COUNT && err == ESP_OK; ++beep)
        {
            size_t frames_left = beep_frames;
            while (frames_left > 0)
            {
                size_t frames = frames_left > SPK_FRAMES_PER_CHUNK ? SPK_FRAMES_PER_CHUNK : frames_left;
                speaker_test_fill_tone(samples, frames, &phase, volume_percent);

                size_t bytes_written = 0;
                err = i2s_channel_write(tx_chan, samples, frames * 2U * sizeof(samples[0]),
                                        &bytes_written, 1000);
                if (err != ESP_OK)
                    break;

                size_t frames_written = bytes_written / (2U * sizeof(samples[0]));
                if (frames_written == 0)
                {
                    err = ESP_ERR_TIMEOUT;
                    break;
                }
                frames_left -= frames_written;
            }

            if (err != ESP_OK || beep + 1U >= SPK_BEEP_COUNT)
                break;

            memset(samples, 0, sizeof(samples));
            frames_left = gap_frames;
            while (frames_left > 0)
            {
                size_t frames = frames_left > SPK_FRAMES_PER_CHUNK ? SPK_FRAMES_PER_CHUNK : frames_left;
                size_t bytes_written = 0;
                err = i2s_channel_write(tx_chan, samples, frames * 2U * sizeof(samples[0]),
                                        &bytes_written, 1000);
                if (err != ESP_OK)
                    break;

                size_t frames_written = bytes_written / (2U * sizeof(samples[0]));
                if (frames_written == 0)
                {
                    err = ESP_ERR_TIMEOUT;
                    break;
                }
                frames_left -= frames_written;
            }
        }

        memset(samples, 0, sizeof(samples));
        size_t bytes_written = 0;
        (void)i2s_channel_write(tx_chan, samples, sizeof(samples), &bytes_written, 1000);
    }

    esp_err_t cleanup_err = i2s_channel_disable(tx_chan);
    if (cleanup_err != ESP_OK && err == ESP_OK)
        err = cleanup_err;

    cleanup_err = i2s_del_channel(tx_chan);
    if (cleanup_err != ESP_OK && err == ESP_OK)
        err = cleanup_err;

    esp_err_t amp_err = speaker_test_set_amp(false);
    if (amp_err != ESP_OK && err == ESP_OK)
        err = amp_err;

    if (err == ESP_OK)
        ESP_LOGI(TAG, "Speaker beep test done; GPIO%d left low", SPK_SD_MODE_GPIO);
    else
        ESP_LOGW(TAG, "Speaker test failed: %s", esp_err_to_name(err));

    return err;
}
