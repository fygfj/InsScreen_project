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

/*
 * 开始一次完整的 I2C 操作。
 *
 * 对于 AHT20 这种“发送命令 -> 等待 -> 读取结果”的器件，调用者必须从发送
 * 命令前一直持有操作锁，读取完成后再调用 i2c_bus_end_operation()。这样进入
 * 深度休眠时，不会在一次测量进行到一半时隔离 SDA/SCL 引脚。
 */
esp_err_t i2c_bus_begin_operation(void);
void i2c_bus_end_operation(void);

/* 禁止新操作、等待当前操作结束，然后把 I2C 引脚切换为低功耗状态。 */
esp_err_t i2c_bus_prepare_sleep(void);

#ifdef __cplusplus
}
#endif
