#include "i2c_bus.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus;
static SemaphoreHandle_t s_lock;
static atomic_bool s_initialized;

static SemaphoreHandle_t i2c_bus_lock(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    return s_lock;
}

esp_err_t i2c_bus_init(void)
{
    if (atomic_load(&s_initialized))
        return ESP_OK;

    SemaphoreHandle_t lock = i2c_bus_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (atomic_load(&s_initialized)) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }

    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = I2C_BUS_SDA_GPIO,
        .scl_io_num = I2C_BUS_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = false,
        },
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err == ESP_OK) {
        atomic_store(&s_initialized, true);
        ESP_LOGI(TAG, "ready: SDA GPIO%d, SCL GPIO%d, port %d",
                 (int)I2C_BUS_SDA_GPIO, (int)I2C_BUS_SCL_GPIO,
                 (int)I2C_BUS_PORT);
    } else {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(lock);
    return err;
}

bool i2c_bus_is_initialized(void)
{
    return atomic_load(&s_initialized);
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    if (i2c_bus_init() != ESP_OK)
        return NULL;
    return s_bus;
}

esp_err_t i2c_bus_probe(uint16_t address, int timeout_ms)
{
    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_probe(bus, address, timeout_ms);
}

esp_err_t i2c_bus_add_device(uint16_t address,
                             uint32_t speed_hz,
                             i2c_master_dev_handle_t *out_dev)
{
    if (!out_dev)
        return ESP_ERR_INVALID_ARG;

    i2c_master_bus_handle_t bus = i2c_bus_get_handle();
    if (!bus)
        return ESP_ERR_INVALID_STATE;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = speed_hz ? speed_hz : I2C_BUS_SPEED_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, out_dev);
}
