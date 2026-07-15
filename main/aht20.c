#include "aht20.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define AHT20_CMD_STATUS       0x71U
#define AHT20_CMD_INIT         0xBEU
#define AHT20_CMD_TRIGGER      0xACU
#define AHT20_CMD_SOFT_RESET   0xBAU

#define AHT20_STATUS_BUSY      0x80U
#define AHT20_STATUS_CAL       0x08U

#define AHT20_POWER_ON_MS      40U
#define AHT20_INIT_WAIT_MS     10U
#define AHT20_MEASURE_WAIT_MS  80U
#define AHT20_POLL_WAIT_MS     10U
#define AHT20_POLL_ATTEMPTS    6U
#define AHT20_XFER_TIMEOUT_MS  100

static const char *TAG = "aht20";

static i2c_master_dev_handle_t s_dev;
static atomic_bool s_initialized;

static esp_err_t aht20_ensure_device(void)
{
    if (s_dev)
        return ESP_OK;
    return i2c_bus_add_device(AHT20_I2C_ADDR, AHT20_I2C_SPEED_HZ, &s_dev);
}

static uint8_t aht20_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ 0x31);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t aht20_read_status(uint8_t *status)
{
    if (!status)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = aht20_ensure_device();
    if (err != ESP_OK)
        return err;

    const uint8_t cmd = AHT20_CMD_STATUS;
    return i2c_master_transmit_receive(s_dev, &cmd, 1, status, 1,
                                       AHT20_XFER_TIMEOUT_MS);
}

esp_err_t aht20_probe(void)
{
    return i2c_bus_probe(AHT20_I2C_ADDR, AHT20_XFER_TIMEOUT_MS);
}

esp_err_t aht20_init(void)
{
    if (atomic_load(&s_initialized))
        return ESP_OK;

    esp_err_t err = aht20_ensure_device();
    if (err != ESP_OK)
        return err;

    vTaskDelay(pdMS_TO_TICKS(AHT20_POWER_ON_MS));

    uint8_t status = 0;
    err = aht20_read_status(&status);
    if (err != ESP_OK)
        return err;

    if ((status & AHT20_STATUS_CAL) == 0) {
        const uint8_t init_cmd[] = { AHT20_CMD_INIT, 0x08, 0x00 };
        err = i2c_master_transmit(s_dev, init_cmd, sizeof(init_cmd),
                                  AHT20_XFER_TIMEOUT_MS);
        if (err != ESP_OK)
            return err;

        vTaskDelay(pdMS_TO_TICKS(AHT20_INIT_WAIT_MS));
        err = aht20_read_status(&status);
        if (err != ESP_OK)
            return err;
        if ((status & AHT20_STATUS_CAL) == 0) {
            ESP_LOGW(TAG, "calibration bit not set, status=0x%02x", status);
            return ESP_ERR_INVALID_STATE;
        }
    }

    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG, "ready at 0x%02x (status=0x%02x)",
             AHT20_I2C_ADDR, status);
    return ESP_OK;
}

bool aht20_is_initialized(void)
{
    return atomic_load(&s_initialized);
}

esp_err_t aht20_soft_reset(void)
{
    esp_err_t err = aht20_ensure_device();
    if (err != ESP_OK)
        return err;

    const uint8_t cmd = AHT20_CMD_SOFT_RESET;
    err = i2c_master_transmit(s_dev, &cmd, 1, AHT20_XFER_TIMEOUT_MS);
    if (err != ESP_OK)
        return err;

    atomic_store(&s_initialized, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    return aht20_init();
}

esp_err_t aht20_read(aht20_sample_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t err = aht20_init();
    if (err != ESP_OK)
        return err;

    const uint8_t trigger_cmd[] = { AHT20_CMD_TRIGGER, 0x33, 0x00 };
    err = i2c_master_transmit(s_dev, trigger_cmd, sizeof(trigger_cmd),
                              AHT20_XFER_TIMEOUT_MS);
    if (err != ESP_OK)
        return err;

    vTaskDelay(pdMS_TO_TICKS(AHT20_MEASURE_WAIT_MS));

    uint8_t data[7] = {0};
    bool ready = false;
    for (uint32_t attempt = 0; attempt < AHT20_POLL_ATTEMPTS; attempt++) {
        err = i2c_master_receive(s_dev, data, sizeof(data),
                                 AHT20_XFER_TIMEOUT_MS);
        if (err != ESP_OK)
            return err;
        if ((data[0] & AHT20_STATUS_BUSY) == 0) {
            ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(AHT20_POLL_WAIT_MS));
    }
    if (!ready)
        return ESP_ERR_TIMEOUT;

    uint8_t crc = aht20_crc8(data, 6);
    if (crc != data[6]) {
        ESP_LOGW(TAG, "crc mismatch: got 0x%02x expected 0x%02x",
                 data[6], crc);
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t raw_h =
        ((uint32_t)data[1] << 12) |
        ((uint32_t)data[2] << 4) |
        ((uint32_t)data[3] >> 4);
    uint32_t raw_t =
        (((uint32_t)data[3] & 0x0FU) << 16) |
        ((uint32_t)data[4] << 8) |
        (uint32_t)data[5];

    out->present = true;
    out->valid = true;
    out->calibrated = (data[0] & AHT20_STATUS_CAL) != 0;
    out->status = data[0];
    out->humidity_percent = ((float)raw_h * 100.0f) / 1048576.0f;
    out->temperature_c = (((float)raw_t * 200.0f) / 1048576.0f) - 50.0f;

    if (out->humidity_percent < 0.0f)
        out->humidity_percent = 0.0f;
    if (out->humidity_percent > 100.0f)
        out->humidity_percent = 100.0f;
    return ESP_OK;
}
