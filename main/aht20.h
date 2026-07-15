#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AHT20_I2C_ADDR       0x38U
#define AHT20_I2C_SPEED_HZ   100000U

typedef struct {
    bool present;
    bool valid;
    bool calibrated;
    float temperature_c;
    float humidity_percent;
    uint8_t status;
} aht20_sample_t;

esp_err_t aht20_init(void);
bool aht20_is_initialized(void);
esp_err_t aht20_probe(void);
esp_err_t aht20_soft_reset(void);
esp_err_t aht20_read(aht20_sample_t *out);

#ifdef __cplusplus
}
#endif
