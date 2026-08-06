#include "music_player.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "sd_card.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_NONSTANDARD_BUT_LOGICAL
#include "minimp3.h"

static const char *TAG = "music";

#define MUSIC_BCLK_GPIO        GPIO_NUM_39
#define MUSIC_LRCLK_GPIO       GPIO_NUM_40
#define MUSIC_DIN_GPIO         GPIO_NUM_41
#define MUSIC_SD_MODE_GPIO     GPIO_NUM_42

#define MUSIC_DEFAULT_VOLUME   35U
#define MUSIC_MIN_SAMPLE_RATE  8000U
#define MUSIC_MAX_SAMPLE_RATE  48000U
#define MUSIC_FRAMES_PER_CHUNK 512U
#define MUSIC_TASK_STACK       24576U
#define MUSIC_TASK_PRIORITY    5U
#define MUSIC_STOP_WAIT_MS     1500U
#define MUSIC_AMP_SETTLE_MS    20U
#define MUSIC_MPEG_BUF_BYTES   (16U * 1024U)
#define MUSIC_MPEG_READ_BYTES  4096U
#define MUSIC_MPEG_SCAN_LIMIT  (512U * 1024U)

#define MUSIC_NVS_NS           "audio"
#define MUSIC_NVS_KEY_VOLUME   "vol"

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t data_offset;
    uint32_t data_size;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t block_align;
} wav_info_t;

typedef struct {
    char path[192];
    char name[96];
} music_task_arg_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static bool s_stop_requested;
static bool s_pause_requested;
static music_player_status_t s_status = {
    .state = MUSIC_PLAYER_STOPPED,
    .volume_percent = MUSIC_DEFAULT_VOLUME,
};

static SemaphoreHandle_t music_lock(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    return s_lock;
}

static uint8_t clamp_volume(uint8_t percent)
{
    if (percent < 1)
        return 1;
    if (percent > 100)
        return 100;
    return percent;
}

static void *music_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(size);
    return p;
}

static void *music_alloc_internal(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(size);
    return p;
}

const char *music_player_state_name(music_player_state_t state)
{
    switch (state) {
    case MUSIC_PLAYER_STOPPED:  return "stopped";
    case MUSIC_PLAYER_PLAYING:  return "playing";
    case MUSIC_PLAYER_PAUSED:   return "paused";
    case MUSIC_PLAYER_STOPPING: return "stopping";
    case MUSIC_PLAYER_ERROR:    return "error";
    default:                    return "unknown";
    }
}

static bool has_extension(const char *filename, const char *ext)
{
    if (!filename || !ext)
        return false;
    const char *dot = strrchr(filename, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ext) == 0;
}

static bool has_wav_extension(const char *filename)
{
    return has_extension(filename, ".wav");
}

static bool has_mpeg_extension(const char *filename)
{
    return has_extension(filename, ".mpa") ||
           has_extension(filename, ".mp3") ||
           has_extension(filename, ".mp2");
}

bool music_player_is_supported_file(const char *filename)
{
    return music_player_valid_filename(filename) &&
           (has_wav_extension(filename) || has_mpeg_extension(filename));
}

bool music_player_valid_filename(const char *filename)
{
    if (!filename || !filename[0] || strlen(filename) >= sizeof(s_status.track))
        return false;
    if (strstr(filename, ".."))
        return false;

    for (const unsigned char *p = (const unsigned char *)filename; *p; p++) {
        if (*p < 0x20 || *p == '/' || *p == '\\' || *p == ':' ||
            *p == '*' || *p == '?' || *p == '"' || *p == '<' ||
            *p == '>' || *p == '|') {
            return false;
        }
    }
    return true;
}

static bool sample_rate_supported(uint32_t sample_rate_hz)
{
    return sample_rate_hz >= MUSIC_MIN_SAMPLE_RATE &&
           sample_rate_hz <= MUSIC_MAX_SAMPLE_RATE;
}

static void status_set_error_locked(esp_err_t err)
{
    s_status.state = MUSIC_PLAYER_ERROR;
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s",
             esp_err_to_name(err));
}

static void status_set_state(music_player_state_t state)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    s_status.state = state;
    xSemaphoreGive(lock);
}

static void status_set_audio_format(uint32_t sample_rate_hz, uint16_t channels,
                                    uint16_t bits_per_sample,
                                    uint64_t data_bytes_total)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    s_status.sample_rate_hz = sample_rate_hz;
    s_status.channels = channels;
    s_status.bits_per_sample = bits_per_sample;
    s_status.data_bytes_total = data_bytes_total;
    s_status.data_bytes_played = 0;
    s_status.elapsed_ms = 0;
    xSemaphoreGive(lock);
}

static void status_set_format(const wav_info_t *info)
{
    status_set_audio_format(info->sample_rate_hz, info->channels,
                            info->bits_per_sample, info->data_size);
}

static void status_update_audio_progress(uint64_t bytes_played,
                                         uint32_t elapsed_ms)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    s_status.data_bytes_played = bytes_played;
    s_status.elapsed_ms = elapsed_ms;
    xSemaphoreGive(lock);
}

static void status_update_progress(uint64_t played, const wav_info_t *info)
{
    uint32_t elapsed_ms = 0;
    if (info->block_align > 0 && info->sample_rate_hz > 0) {
        uint64_t frames = played / info->block_align;
        elapsed_ms = (uint32_t)((frames * 1000ULL) / info->sample_rate_hz);
    }
    status_update_audio_progress(played, elapsed_ms);
}

static void get_controls(bool *stop, bool *pause, uint8_t *volume)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (stop)
        *stop = s_stop_requested;
    if (pause)
        *pause = s_pause_requested;
    if (volume)
        *volume = s_status.volume_percent;
    xSemaphoreGive(lock);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool fread_exact(FILE *f, void *buf, size_t len)
{
    return fread(buf, 1, len, f) == len;
}

static esp_err_t skip_bytes(FILE *f, uint32_t bytes)
{
    if (bytes == 0)
        return ESP_OK;
    return fseek(f, (long)bytes, SEEK_CUR) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t parse_wav(FILE *f, wav_info_t *out)
{
    uint8_t hdr[12];
    if (!f || !out || !fread_exact(f, hdr, sizeof(hdr)))
        return ESP_ERR_INVALID_SIZE;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return ESP_ERR_INVALID_ARG;

    bool have_fmt = false;
    bool have_data = false;
    memset(out, 0, sizeof(*out));

    for (;;) {
        uint8_t chdr[8];
        if (!fread_exact(f, chdr, sizeof(chdr)))
            break;

        uint32_t chunk_size = le32(chdr + 4);
        long payload_pos = ftell(f);
        if (payload_pos < 0)
            return ESP_FAIL;

        if (memcmp(chdr, "fmt ", 4) == 0) {
            if (chunk_size < 16)
                return ESP_ERR_INVALID_ARG;
            uint8_t fmt[40] = {0};
            size_t to_read = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
            if (!fread_exact(f, fmt, to_read))
                return ESP_ERR_INVALID_SIZE;

            uint16_t audio_format = le16(fmt + 0);
            out->channels = le16(fmt + 2);
            out->sample_rate_hz = le32(fmt + 4);
            out->block_align = le16(fmt + 12);
            out->bits_per_sample = le16(fmt + 14);

            if (chunk_size > to_read &&
                skip_bytes(f, chunk_size - (uint32_t)to_read) != ESP_OK) {
                return ESP_FAIL;
            }

            if (audio_format != 1 ||
                (out->channels != 1 && out->channels != 2) ||
                out->bits_per_sample != 16 ||
                out->block_align != out->channels * 2 ||
                out->sample_rate_hz < MUSIC_MIN_SAMPLE_RATE ||
                out->sample_rate_hz > MUSIC_MAX_SAMPLE_RATE) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            have_fmt = true;
        } else if (memcmp(chdr, "data", 4) == 0) {
            out->data_offset = (uint32_t)payload_pos;
            out->data_size = chunk_size;
            have_data = true;
            if (skip_bytes(f, chunk_size) != ESP_OK)
                return ESP_FAIL;
        } else {
            if (skip_bytes(f, chunk_size) != ESP_OK)
                return ESP_FAIL;
        }

        if (chunk_size & 1U) {
            if (skip_bytes(f, 1) != ESP_OK)
                return ESP_FAIL;
        }

        if (have_fmt && have_data)
            break;
    }

    if (!have_fmt || !have_data || out->data_size == 0)
        return ESP_ERR_NOT_FOUND;

    if (fseek(f, (long)out->data_offset, SEEK_SET) != 0)
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t music_player_validate_wav_file(const char *path)
{
    if (!path || !path[0])
        return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f)
        return ESP_ERR_NOT_FOUND;

    wav_info_t info;
    esp_err_t err = parse_wav(f, &info);
    fclose(f);
    if (err == ESP_OK) {
        struct stat st;
        if (stat(path, &st) != 0 ||
            (uint64_t)info.data_offset + info.data_size > (uint64_t)st.st_size) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    return err;
}

static esp_err_t music_player_validate_mpeg_file(const char *path)
{
    if (!path || !path[0])
        return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f)
        return ESP_ERR_NOT_FOUND;

    uint8_t *input = (uint8_t *)music_alloc(MUSIC_MPEG_BUF_BYTES);
    mp3d_sample_t *pcm = (mp3d_sample_t *)music_alloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));
    if (!input || !pcm) {
        free(input);
        free(pcm);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    mp3dec_t *dec = (mp3dec_t *)music_alloc(sizeof(*dec));
    if (!dec) {
        free(input);
        free(pcm);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    mp3dec_init(dec);
    size_t input_size = 0;
    size_t total_read = 0;
    bool eof = false;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    while (total_read < MUSIC_MPEG_SCAN_LIMIT) {
        if (!eof && input_size < MUSIC_MPEG_BUF_BYTES) {
            size_t room = MUSIC_MPEG_BUF_BYTES - input_size;
            size_t left = MUSIC_MPEG_SCAN_LIMIT - total_read;
            size_t want = room < left ? room : left;
            size_t got = fread(input + input_size, 1, want, f);
            input_size += got;
            total_read += got;
            if (got < want) {
                if (ferror(f))
                    err = ESP_FAIL;
                eof = true;
            }
        }

        if (input_size == 0) {
            if (eof)
                break;
            continue;
        }

        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(dec, input, (int)input_size,
                                          pcm, &info);
        if (info.frame_bytes <= 0) {
            if (eof || input_size == MUSIC_MPEG_BUF_BYTES)
                break;
            continue;
        }

        size_t used = (size_t)info.frame_bytes;
        if (used > input_size)
            used = input_size;
        if (samples > 0) {
            if (!sample_rate_supported((uint32_t)info.hz) ||
                (info.channels != 1 && info.channels != 2) ||
                info.layer < 1 || info.layer > 3) {
                err = ESP_ERR_NOT_SUPPORTED;
            } else {
                err = ESP_OK;
            }
            break;
        }

        memmove(input, input + used, input_size - used);
        input_size -= used;
    }

    free(input);
    free(pcm);
    free(dec);
    fclose(f);
    return err;
}

esp_err_t music_player_validate_file(const char *path, const char *filename)
{
    if (!path || !filename)
        return ESP_ERR_INVALID_ARG;
    if (has_wav_extension(filename))
        return music_player_validate_wav_file(path);
    if (has_mpeg_extension(filename))
        return music_player_validate_mpeg_file(path);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t amp_set_enabled(bool enabled)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << MUSIC_SD_MODE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK)
        return err;
    return gpio_set_level(MUSIC_SD_MODE_GPIO, enabled ? 1 : 0);
}

static esp_err_t i2s_open(uint32_t sample_rate_hz, i2s_chan_handle_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;

    esp_err_t err = amp_set_enabled(true);
    if (err != ESP_OK)
        return err;
    vTaskDelay(pdMS_TO_TICKS(MUSIC_AMP_SETTLE_MS));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,
                                                            I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;

    i2s_chan_handle_t tx = NULL;
    err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) {
        (void)amp_set_enabled(false);
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MUSIC_BCLK_GPIO,
            .ws = MUSIC_LRCLK_GPIO,
            .dout = MUSIC_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err == ESP_OK)
        err = i2s_channel_enable(tx);
    if (err != ESP_OK) {
        (void)i2s_del_channel(tx);
        (void)amp_set_enabled(false);
        return err;
    }

    *out = tx;
    return ESP_OK;
}

static void i2s_close(i2s_chan_handle_t tx)
{
    if (tx) {
        int16_t zeros[MUSIC_FRAMES_PER_CHUNK * 2] = {0};
        size_t written = 0;
        (void)i2s_channel_write(tx, zeros, sizeof(zeros), &written, 200);
        (void)i2s_channel_disable(tx);
        (void)i2s_del_channel(tx);
    }
    (void)amp_set_enabled(false);
}

static int16_t scale_sample(int16_t sample, uint8_t volume)
{
    int32_t v = ((int32_t)sample * (int32_t)volume) / 100;
    if (v > INT16_MAX)
        v = INT16_MAX;
    if (v < INT16_MIN)
        v = INT16_MIN;
    return (int16_t)v;
}

static esp_err_t write_all_i2s(i2s_chan_handle_t tx, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t done = 0;
    while (done < len) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx, p + done, len - done,
                                          &written, 1000);
        if (err != ESP_OK)
            return err;
        if (written == 0)
            return ESP_ERR_TIMEOUT;
        done += written;
    }
    return ESP_OK;
}

static esp_err_t write_stereo_pcm(i2s_chan_handle_t tx, const int16_t *src,
                                  size_t frames, uint16_t channels,
                                  int16_t *pcm)
{
    if (!tx || !src || !pcm || (channels != 1 && channels != 2))
        return ESP_ERR_INVALID_ARG;

    uint8_t volume = MUSIC_DEFAULT_VOLUME;
    get_controls(NULL, NULL, &volume);
    if (channels == 1) {
        for (size_t i = 0; i < frames; i++) {
            int16_t s = scale_sample(src[i], volume);
            pcm[i * 2U] = s;
            pcm[i * 2U + 1U] = s;
        }
    } else {
        for (size_t i = 0; i < frames; i++) {
            pcm[i * 2U] = scale_sample(src[i * 2U], volume);
            pcm[i * 2U + 1U] = scale_sample(src[i * 2U + 1U], volume);
        }
    }
    return write_all_i2s(tx, pcm, frames * 2U * sizeof(int16_t));
}

static esp_err_t wait_if_paused_or_stopped(void)
{
    for (;;) {
        bool stop = false;
        bool pause = false;
        get_controls(&stop, &pause, NULL);
        if (stop)
            return ESP_ERR_INVALID_STATE;
        if (!pause) {
            status_set_state(MUSIC_PLAYER_PLAYING);
            return ESP_OK;
        }
        status_set_state(MUSIC_PLAYER_PAUSED);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static esp_err_t play_wav_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "open failed: %s errno=%d", path, errno);
        return ESP_ERR_NOT_FOUND;
    }

    wav_info_t info;
    esp_err_t err = parse_wav(f, &info);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    ESP_LOGI(TAG, "WAV: %lu Hz, %u ch, %u bit, %lu bytes",
             (unsigned long)info.sample_rate_hz,
             (unsigned)info.channels,
             (unsigned)info.bits_per_sample,
             (unsigned long)info.data_size);
    status_set_format(&info);

    i2s_chan_handle_t tx = NULL;
    err = i2s_open(info.sample_rate_hz, &tx);
    if (err != ESP_OK) {
        fclose(f);
        return err;
    }

    const size_t in_frame_bytes = info.block_align;
    const size_t raw_len = MUSIC_FRAMES_PER_CHUNK * in_frame_bytes;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    int16_t *pcm = (int16_t *)malloc(MUSIC_FRAMES_PER_CHUNK * 2U * sizeof(int16_t));
    if (!raw || !pcm) {
        free(raw);
        free(pcm);
        i2s_close(tx);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    uint64_t played = 0;
    uint32_t remaining = info.data_size;
    while (remaining >= in_frame_bytes) {
        err = wait_if_paused_or_stopped();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
            break;
        }
        if (err != ESP_OK)
            break;

        uint32_t to_read = remaining > raw_len ? (uint32_t)raw_len : remaining;
        to_read -= to_read % in_frame_bytes;
        if (to_read == 0)
            break;

        size_t got = fread(raw, 1, to_read, f);
        if (got == 0) {
            err = ferror(f) ? ESP_FAIL : ESP_OK;
            break;
        }
        got -= got % in_frame_bytes;
        if (got == 0)
            break;

        size_t frames = got / in_frame_bytes;
        err = write_stereo_pcm(tx, (const int16_t *)raw, frames,
                               info.channels, pcm);
        if (err != ESP_OK)
            break;

        played += got;
        remaining -= (uint32_t)got;
        status_update_progress(played, &info);
    }

    free(raw);
    free(pcm);
    i2s_close(tx);
    fclose(f);
    return err;
}

static esp_err_t play_mpeg_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "open MPEG audio failed: %s errno=%d", path, errno);
        return ESP_ERR_NOT_FOUND;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fclose(f);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *input = (uint8_t *)music_alloc(MUSIC_MPEG_BUF_BYTES);
    mp3d_sample_t *decoded = (mp3d_sample_t *)music_alloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));
    int16_t *pcm = (int16_t *)music_alloc_internal(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    mp3dec_t *dec = (mp3dec_t *)music_alloc(sizeof(*dec));
    if (!input || !decoded || !pcm || !dec) {
        free(input);
        free(decoded);
        free(pcm);
        free(dec);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    mp3dec_init(dec);
    i2s_chan_handle_t tx = NULL;
    size_t input_size = 0;
    bool eof = false;
    bool format_ready = false;
    uint32_t sample_rate_hz = 0;
    uint16_t channels = 0;
    uint64_t stream_pos = 0;
    uint64_t decoded_frames = 0;
    esp_err_t err = ESP_OK;

    for (;;) {
        err = wait_if_paused_or_stopped();
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK;
            break;
        }
        if (err != ESP_OK)
            break;

        if (!eof && input_size < MUSIC_MPEG_BUF_BYTES) {
            size_t room = MUSIC_MPEG_BUF_BYTES - input_size;
            size_t want = room < MUSIC_MPEG_READ_BYTES ? room : MUSIC_MPEG_READ_BYTES;
            size_t got = fread(input + input_size, 1, want, f);
            input_size += got;
            if (got < want) {
                if (ferror(f)) {
                    err = ESP_FAIL;
                    break;
                }
                eof = true;
            }
        }

        if (input_size == 0) {
            if (eof)
                break;
            continue;
        }

        mp3dec_frame_info_t info = {0};
        int samples = mp3dec_decode_frame(dec, input, (int)input_size,
                                          decoded, &info);
        if (info.frame_bytes <= 0) {
            if (eof || input_size == MUSIC_MPEG_BUF_BYTES) {
                err = decoded_frames > 0 ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
                break;
            }
            continue;
        }

        size_t used = (size_t)info.frame_bytes;
        if (used > input_size)
            used = input_size;

        if (samples > 0) {
            if (!sample_rate_supported((uint32_t)info.hz) ||
                (info.channels != 1 && info.channels != 2) ||
                info.layer < 1 || info.layer > 3) {
                err = ESP_ERR_NOT_SUPPORTED;
                break;
            }

            if (!format_ready) {
                sample_rate_hz = (uint32_t)info.hz;
                channels = (uint16_t)info.channels;
                status_set_audio_format(sample_rate_hz, channels, 16,
                                        (uint64_t)st.st_size);
                err = i2s_open(sample_rate_hz, &tx);
                if (err != ESP_OK)
                    break;
                format_ready = true;
                ESP_LOGI(TAG, "MPEG: %lu Hz, %u ch, layer %d, %llu bytes",
                         (unsigned long)sample_rate_hz,
                         (unsigned)channels,
                         info.layer,
                         (unsigned long long)st.st_size);
            } else if (sample_rate_hz != (uint32_t)info.hz ||
                       channels != (uint16_t)info.channels) {
                err = ESP_ERR_NOT_SUPPORTED;
                break;
            }

            err = write_stereo_pcm(tx, (const int16_t *)decoded,
                                   (size_t)samples, channels, pcm);
            if (err != ESP_OK)
                break;
            decoded_frames += (uint64_t)samples;
        }

        memmove(input, input + used, input_size - used);
        input_size -= used;
        stream_pos += used;
        if (format_ready) {
            uint32_t elapsed_ms = (uint32_t)((decoded_frames * 1000ULL) /
                                             sample_rate_hz);
            status_update_audio_progress(stream_pos, elapsed_ms);
        }
    }

    if (err == ESP_OK && !format_ready)
        err = ESP_ERR_NOT_SUPPORTED;
    free(input);
    free(decoded);
    free(pcm);
    free(dec);
    i2s_close(tx);
    fclose(f);
    return err;
}

static void music_task(void *arg)
{
    music_task_arg_t *task_arg = (music_task_arg_t *)arg;
    esp_err_t err = has_wav_extension(task_arg->name)
                        ? play_wav_file(task_arg->path)
                        : play_mpeg_file(task_arg->path);

    SemaphoreHandle_t lock = music_lock();
    if (lock) {
        xSemaphoreTake(lock, portMAX_DELAY);
        s_task = NULL;
        s_stop_requested = false;
        s_pause_requested = false;
        if (err == ESP_OK) {
            s_status.state = MUSIC_PLAYER_STOPPED;
        } else {
            status_set_error_locked(err);
            ESP_LOGW(TAG, "playback failed: %s", esp_err_to_name(err));
        }
        xSemaphoreGive(lock);
    }

    free(task_arg);
    vTaskDelete(NULL);
}

esp_err_t music_player_init(void)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    uint8_t volume = MUSIC_DEFAULT_VOLUME;
    nvs_handle_t h;
    if (nvs_open(MUSIC_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t saved = 0;
        if (nvs_get_u8(h, MUSIC_NVS_KEY_VOLUME, &saved) == ESP_OK)
            volume = clamp_volume(saved);
        nvs_close(h);
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    s_status.state = MUSIC_PLAYER_STOPPED;
    s_status.volume_percent = volume;
    s_status.last_error[0] = '\0';
    xSemaphoreGive(lock);

    (void)amp_set_enabled(false);
    ESP_LOGI(TAG, "ready on BCLK=%d LRCLK=%d DIN=%d MODE=%d volume=%u%%",
             MUSIC_BCLK_GPIO, MUSIC_LRCLK_GPIO, MUSIC_DIN_GPIO,
             MUSIC_SD_MODE_GPIO, (unsigned)volume);
    return ESP_OK;
}

esp_err_t music_player_set_volume(uint8_t percent)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    percent = clamp_volume(percent);
    xSemaphoreTake(lock, portMAX_DELAY);
    s_status.volume_percent = percent;
    xSemaphoreGive(lock);

    nvs_handle_t h;
    esp_err_t err = nvs_open(MUSIC_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_u8(h, MUSIC_NVS_KEY_VOLUME, percent);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t music_player_stop(void)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    xSemaphoreTake(lock, portMAX_DELAY);
    TaskHandle_t task = s_task;
    if (!task) {
        s_stop_requested = false;
        s_pause_requested = false;
        s_status.state = MUSIC_PLAYER_STOPPED;
        s_status.last_error[0] = '\0';
        xSemaphoreGive(lock);
        return ESP_OK;
    }
    s_stop_requested = true;
    s_pause_requested = false;
    s_status.state = MUSIC_PLAYER_STOPPING;
    xSemaphoreGive(lock);

    const TickType_t step = pdMS_TO_TICKS(20);
    const int loops = MUSIC_STOP_WAIT_MS / 20U;
    for (int i = 0; i < loops; i++) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool done = (s_task == NULL);
        xSemaphoreGive(lock);
        if (done)
            return ESP_OK;
        vTaskDelay(step);
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t music_player_play(const char *filename)
{
    if (!music_player_is_supported_file(filename))
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = sd_card_mount();
    if (err != ESP_OK)
        return err;

    music_task_arg_t *arg = calloc(1, sizeof(*arg));
    if (!arg)
        return ESP_ERR_NO_MEM;

    snprintf(arg->name, sizeof(arg->name), "%s", filename);
    snprintf(arg->path, sizeof(arg->path), "%s/%s", SD_CARD_MUSIC_DIR, filename);

    struct stat st;
    if (stat(arg->path, &st) != 0) {
        free(arg);
        return ESP_ERR_NOT_FOUND;
    }

    err = music_player_stop();
    if (err != ESP_OK) {
        free(arg);
        return err;
    }

    SemaphoreHandle_t lock = music_lock();
    if (!lock) {
        free(arg);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    s_stop_requested = false;
    s_pause_requested = false;
    uint8_t volume = s_status.volume_percent;
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = MUSIC_PLAYER_PLAYING;
    s_status.volume_percent = clamp_volume(volume ? volume : MUSIC_DEFAULT_VOLUME);
    snprintf(s_status.track, sizeof(s_status.track), "%s", filename);

    TaskHandle_t task = NULL;
    BaseType_t ok = xTaskCreate(music_task, "music_play", MUSIC_TASK_STACK,
                                arg, MUSIC_TASK_PRIORITY, &task);
    if (ok != pdPASS) {
        s_status.state = MUSIC_PLAYER_STOPPED;
        xSemaphoreGive(lock);
        free(arg);
        return ESP_ERR_NO_MEM;
    }
    s_task = task;
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t music_player_pause(void)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!s_task || s_status.state == MUSIC_PLAYER_STOPPING) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_pause_requested = true;
    s_status.state = MUSIC_PLAYER_PAUSED;
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t music_player_resume(void)
{
    SemaphoreHandle_t lock = music_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!s_task || s_status.state == MUSIC_PLAYER_STOPPING) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_pause_requested = false;
    s_status.state = MUSIC_PLAYER_PLAYING;
    xSemaphoreGive(lock);
    return ESP_OK;
}

void music_player_get_status(music_player_status_t *out)
{
    if (!out)
        return;
    SemaphoreHandle_t lock = music_lock();
    if (!lock) {
        memset(out, 0, sizeof(*out));
        out->state = MUSIC_PLAYER_ERROR;
        snprintf(out->last_error, sizeof(out->last_error), "%s",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(lock);
}
