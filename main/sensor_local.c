#include "sensor_local.h"

#include <stdatomic.h>
#include <string.h>

#include "aht20.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define SENSOR_NVS_NS           "sensor"
#define SENSOR_NVS_KEY_ENABLED  "en"
#define SENSOR_NVS_KEY_CLOCK    "clock"
#define SENSOR_NVS_KEY_WEATHER  "weather"
#define SENSOR_NVS_KEY_CALENDAR "calendar"

static const char *TAG = "sensor";

static SemaphoreHandle_t s_mutex;
static atomic_bool s_loaded;
static atomic_bool s_initialized;

static sensor_local_config_t s_cfg = {
    .enabled = true,
    .show_on_clock = true,
    .show_on_weather = true,
    .show_on_calendar = false,
};

static sensor_local_data_t s_data = {
    .enabled = true,
    .present = false,
    .valid = false,
    .calibrated = false,
    .temperature_c = 0.0f,
    .humidity_percent = 0.0f,
    .updated_ms = 0,
    .age_ms = -1,
    .last_error = ESP_ERR_INVALID_STATE,
    .status = 0,
    .i2c_addr = 0,
};

static int64_t sensor_now_ms(void)
{
    return esp_timer_get_time() / 1000LL;
}

static sensor_local_config_t sensor_validate_config(const sensor_local_config_t *cfg)
{
    sensor_local_config_t out = {
        .enabled = true,
        .show_on_clock = true,
        .show_on_weather = true,
        .show_on_calendar = false,
    };
    if (cfg)
        out = *cfg;
    return out;
}

static void sensor_update_age_locked(void)
{
    s_data.enabled = s_cfg.enabled;
    if (s_data.updated_ms > 0) {
        int64_t age = sensor_now_ms() - s_data.updated_ms;
        s_data.age_ms = age >= 0 ? age : 0;
    } else {
        s_data.age_ms = -1;
    }
}

static esp_err_t sensor_save_config_locked(const sensor_local_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SENSOR_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    err = nvs_set_u8(h, SENSOR_NVS_KEY_ENABLED, cfg->enabled ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_u8(h, SENSOR_NVS_KEY_CLOCK, cfg->show_on_clock ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_u8(h, SENSOR_NVS_KEY_WEATHER, cfg->show_on_weather ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_set_u8(h, SENSOR_NVS_KEY_CALENDAR, cfg->show_on_calendar ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);
    return err;
}

static void sensor_load_config_once(void)
{
    if (atomic_load(&s_loaded))
        return;

    sensor_local_config_t cfg = sensor_validate_config(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(SENSOR_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v8 = 0;
        if (nvs_get_u8(h, SENSOR_NVS_KEY_ENABLED, &v8) == ESP_OK)
            cfg.enabled = (v8 != 0);
        if (nvs_get_u8(h, SENSOR_NVS_KEY_CLOCK, &v8) == ESP_OK)
            cfg.show_on_clock = (v8 != 0);
        if (nvs_get_u8(h, SENSOR_NVS_KEY_WEATHER, &v8) == ESP_OK)
            cfg.show_on_weather = (v8 != 0);
        if (nvs_get_u8(h, SENSOR_NVS_KEY_CALENDAR, &v8) == ESP_OK)
            cfg.show_on_calendar = (v8 != 0);
        nvs_close(h);
    }

    s_cfg = sensor_validate_config(&cfg);
    s_data.enabled = s_cfg.enabled;
    atomic_store(&s_loaded, true);
}

static esp_err_t sensor_take_mutex(void)
{
    if (!s_mutex)
        s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sensor_local_init(void)
{
    esp_err_t err = sensor_take_mutex();
    if (err != ESP_OK)
        return err;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_load_config_once();
    sensor_update_age_locked();
    atomic_store(&s_initialized, true);
    bool enabled = s_cfg.enabled;
    xSemaphoreGive(s_mutex);

    if (enabled) {
        sensor_local_data_t ignored;
        err = sensor_local_read_now(&ignored);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "AHTx0 initial read failed: %s",
                     sensor_local_error_name(err));
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "ready: enabled=%d SDA=GPIO%d SCL=GPIO%d addr=0x%02x",
             enabled, SENSOR_LOCAL_SDA_GPIO, SENSOR_LOCAL_SCL_GPIO,
             aht20_get_address());
    return ESP_OK;
}

esp_err_t sensor_local_get_config(sensor_local_config_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = sensor_take_mutex();
    if (err != ESP_OK)
        return err;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_load_config_once();
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t sensor_local_set_config(const sensor_local_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = sensor_take_mutex();
    if (err != ESP_OK)
        return err;

    sensor_local_config_t validated = sensor_validate_config(cfg);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_load_config_once();
    s_cfg = validated;
    s_data.enabled = s_cfg.enabled;
    if (!s_cfg.enabled) {
        s_data.valid = false;
        s_data.last_error = ESP_ERR_INVALID_STATE;
        sensor_update_age_locked();
    }
    err = sensor_save_config_locked(&s_cfg);
    xSemaphoreGive(s_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "config updated: enabled=%d clock=%d weather=%d calendar=%d",
             s_cfg.enabled, s_cfg.show_on_clock, s_cfg.show_on_weather,
             s_cfg.show_on_calendar);
    return ESP_OK;
}

esp_err_t sensor_local_read_now(sensor_local_data_t *out)
{
    esp_err_t err = sensor_take_mutex();
    if (err != ESP_OK)
        return err;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_load_config_once();
    if (!s_cfg.enabled) {
        s_data.enabled = false;
        s_data.present = false;
        s_data.valid = false;
        s_data.calibrated = false;
        s_data.status = 0;
        s_data.i2c_addr = aht20_get_address();
        s_data.last_error = ESP_ERR_INVALID_STATE;
        sensor_update_age_locked();
        if (out)
            *out = s_data;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    aht20_sample_t sample = {0};
    err = aht20_read(&sample);
    s_data.enabled = true;
    s_data.last_error = err;
    s_data.i2c_addr = sample.i2c_addr ? sample.i2c_addr : aht20_get_address();
    if (err == ESP_OK && sample.valid) {
        s_data.present = true;
        s_data.valid = true;
        s_data.calibrated = sample.calibrated;
        s_data.temperature_c = sample.temperature_c;
        s_data.humidity_percent = sample.humidity_percent;
        s_data.status = sample.status;
        s_data.updated_ms = sensor_now_ms();
    } else {
        s_data.present = false;
        s_data.valid = false;
        s_data.calibrated = false;
        s_data.status = sample.status;
    }
    sensor_update_age_locked();
    if (out)
        *out = s_data;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t sensor_local_ensure_fresh(uint32_t max_age_ms)
{
    esp_err_t err = sensor_take_mutex();
    if (err != ESP_OK)
        return err;

    bool need_read = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_load_config_once();
    sensor_update_age_locked();
    if (!s_cfg.enabled) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_data.valid || s_data.age_ms < 0 ||
        (uint64_t)s_data.age_ms > (uint64_t)max_age_ms) {
        need_read = true;
    }
    xSemaphoreGive(s_mutex);

    return need_read ? sensor_local_read_now(NULL) : ESP_OK;
}

esp_err_t sensor_local_get_data(sensor_local_data_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = sensor_take_mutex();
    if (err != ESP_OK)
        return err;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_load_config_once();
    sensor_update_age_locked();
    *out = s_data;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

const char *sensor_local_error_name(esp_err_t err)
{
    if (err == ESP_OK)
        return "";
    const char *name = esp_err_to_name(err);
    return name ? name : "ESP_FAIL";
}
