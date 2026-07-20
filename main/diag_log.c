#include "diag_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "battery_mon.h"
#include "sd_card.h"
#include "sensor_local.h"
#include "time_sync.h"

#define DIAG_LOG_BUFFER_BYTES (16 * 1024)
#define DIAG_LOG_EXPORT_MAX_BYTES (96 * 1024)

static const char *TAG = "diag_log";

static char s_buf[DIAG_LOG_BUFFER_BYTES];
static size_t s_head;
static size_t s_used;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev_vprintf;
static bool s_ready;
static bool s_in_hook;

static void diag_log_append_unlocked(const char *data, size_t len)
{
    if (!data || len == 0)
        return;

    if (len >= DIAG_LOG_BUFFER_BYTES) {
        data += len - (DIAG_LOG_BUFFER_BYTES - 1);
        len = DIAG_LOG_BUFFER_BYTES - 1;
    }

    size_t tail = (s_head + s_used) % DIAG_LOG_BUFFER_BYTES;
    for (size_t i = 0; i < len; i++) {
        if (s_used == DIAG_LOG_BUFFER_BYTES) {
            s_head = (s_head + 1) % DIAG_LOG_BUFFER_BYTES;
            s_used--;
        }
        s_buf[tail] = data[i];
        tail = (tail + 1) % DIAG_LOG_BUFFER_BYTES;
        s_used++;
    }
}

static int diag_log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);

    if (!s_in_hook && s_ready && s_lock) {
        s_in_hook = true;
        char line[384];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0) {
            size_t len = (size_t)n;
            if (len >= sizeof(line))
                len = sizeof(line) - 1;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
                diag_log_append_unlocked(line, len);
                xSemaphoreGive(s_lock);
            }
        }
        s_in_hook = false;
    }

    va_end(copy);
    return ret;
}

void diag_log_init(void)
{
    if (s_ready)
        return;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return;

    s_prev_vprintf = esp_log_set_vprintf(diag_log_vprintf);
    s_ready = true;
    ESP_LOGI(TAG, "ready: recent log buffer %u bytes", (unsigned)DIAG_LOG_BUFFER_BYTES);
}

size_t diag_log_buffer_used(void)
{
    if (!s_ready || !s_lock)
        return 0;

    size_t used = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        used = s_used;
        xSemaphoreGive(s_lock);
    }
    return used;
}

static void diag_log_make_path(char *out, size_t out_len)
{
    struct tm tm;
    if (time_sync_get_local_relaxed(&tm)) {
        snprintf(out, out_len, "%s/diag_%04d%02d%02d_%02d%02d%02d.log",
                 SD_CARD_LOGS_DIR,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(out, out_len, "%s/diag_%lu.log",
                 SD_CARD_LOGS_DIR, (unsigned long)(esp_log_timestamp() / 1000));
    }
}

static esp_err_t diag_log_write_header(FILE *f)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    battery_status_t bat = {0};
    battery_mon_get_status(&bat);
    sensor_local_data_t sensor = {0};
    sensor_local_get_data(&sensor);
    sd_card_status_t sd = {0};
    sd_card_get_status(&sd);

    char time_str[32] = "";
    time_sync_get_str(time_str, sizeof(time_str));

    int n = fprintf(f,
                    "EPD diagnostic log\n"
                    "time=%s synced=%d uptime_ms=%lu\n"
                    "app=%s version=%s idf=%s partition=%s @0x%lx\n"
                    "heap_free=%lu internal_free=%lu psram_free=%lu\n"
                    "battery_valid=%d percent=%u voltage_mv=%d charging=%d\n"
                    "sensor_enabled=%d present=%d valid=%d temp=%.2f humidity=%.2f error=%s\n"
                    "sd_mounted=%d dirs_ready=%d total=%llu free=%llu name=%s error=%s dir_error=%s\n"
                    "\n--- recent log ---\n",
                    time_str[0] ? time_str : "unknown",
                    time_sync_is_synced() ? 1 : 0,
                    (unsigned long)esp_log_timestamp(),
                    desc ? desc->project_name : "",
                    desc ? desc->version : "",
                    desc ? desc->idf_ver : "",
                    running ? running->label : "unknown",
                    running ? (unsigned long)running->address : 0UL,
                    (unsigned long)esp_get_free_heap_size(),
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    bat.valid ? 1 : 0,
                    (unsigned)bat.percent,
                    bat.voltage_mv,
                    bat.charging ? 1 : 0,
                    sensor.enabled ? 1 : 0,
                    sensor.present ? 1 : 0,
                    sensor.valid ? 1 : 0,
                    sensor.valid ? sensor.temperature_c : 0.0f,
                    sensor.valid ? sensor.humidity_percent : 0.0f,
                    sensor_local_error_name(sensor.last_error),
                    sd.mounted ? 1 : 0,
                    sd.dirs_ready ? 1 : 0,
                    (unsigned long long)sd.total_bytes,
                    (unsigned long long)sd.free_bytes,
                    sd.card_name,
                    esp_err_to_name(sd.last_error),
                    esp_err_to_name(sd.last_dir_error));
    return n > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t diag_log_export_to_sd(char *path_out, size_t path_out_len, size_t *bytes_out)
{
    if (bytes_out)
        *bytes_out = 0;

    esp_err_t err = sd_card_mount();
    if (err != ESP_OK)
        return err;

    char path[128];
    diag_log_make_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f)
        return ESP_FAIL;

    size_t bytes = 0;
    err = diag_log_write_header(f);
    if (err == ESP_OK) {
        long pos = ftell(f);
        if (pos > 0)
            bytes = (size_t)pos;
    }

    if (err == ESP_OK && s_ready && s_lock &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t written = 0;
        for (size_t i = 0; i < s_used && bytes + written < DIAG_LOG_EXPORT_MAX_BYTES; i++) {
            char c = s_buf[(s_head + i) % DIAG_LOG_BUFFER_BYTES];
            if (fwrite(&c, 1, 1, f) != 1) {
                err = ESP_FAIL;
                break;
            }
            written++;
        }
        bytes += written;
        xSemaphoreGive(s_lock);
    }

    if (fclose(f) != 0 && err == ESP_OK)
        err = ESP_FAIL;

    if (err != ESP_OK) {
        remove(path);
        return err;
    }

    if (path_out && path_out_len)
        snprintf(path_out, path_out_len, "%s", path);
    if (bytes_out)
        *bytes_out = bytes;

    ESP_LOGI(TAG, "exported diagnostic log: %s (%u bytes)", path, (unsigned)bytes);
    return ESP_OK;
}
