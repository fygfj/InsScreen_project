#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_BUS_PORT       I2C_NUM_0
#define I2C_BUS_SDA_GPIO   GPIO_NUM_1
#define I2C_BUS_SCL_GPIO   GPIO_NUM_2
#define I2C_BUS_SPEED_HZ   100000U

esp_err_t i2c_bus_init(void);
bool i2c_bus_is_initialized(void);
i2c_master_bus_handle_t i2c_bus_get_handle(void);
esp_err_t i2c_bus_probe(uint16_t address, int timeout_ms);
esp_err_t i2c_bus_add_device(uint16_t address,
                             uint32_t speed_hz,
                             i2c_master_dev_handle_t *out_dev);
esp_err_t i2c_bus_prepare_sleep(void);

#ifdef __cplusplus
}
#endif
