#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_LOCAL_SDA_GPIO 1
#define SENSOR_LOCAL_SCL_GPIO 2
#define SENSOR_LOCAL_I2C_ADDR 0x38U
#define SENSOR_LOCAL_DISPLAY_MAX_AGE_MS 120000U

typedef struct {
    bool enabled;
    bool show_on_clock;
    bool show_on_weather;
    bool show_on_calendar;
} sensor_local_config_t;

typedef struct {
    bool enabled;
    bool present;
    bool valid;
    bool calibrated;
    float temperature_c;
    float humidity_percent;
    int64_t updated_ms;
    int64_t age_ms;
    esp_err_t last_error;
    uint8_t status;
    uint8_t i2c_addr;
} sensor_local_data_t;

esp_err_t sensor_local_init(void);

esp_err_t sensor_local_get_config(sensor_local_config_t *out);
esp_err_t sensor_local_set_config(const sensor_local_config_t *cfg);

esp_err_t sensor_local_read_now(sensor_local_data_t *out);
esp_err_t sensor_local_ensure_fresh(uint32_t max_age_ms);
esp_err_t sensor_local_get_data(sensor_local_data_t *out);

const char *sensor_local_error_name(esp_err_t err);

#ifdef __cplusplus
}
#endif
