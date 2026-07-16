#include "aht20.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define AHTX0_CMD_STATUS       0x71U
#define AHTX0_CMD_INIT_AHT10   0xE1U
#define AHTX0_CMD_INIT_AHT20   0xBEU
#define AHTX0_CMD_TRIGGER      0xACU
#define AHTX0_CMD_SOFT_RESET   0xBAU

#define AHTX0_STATUS_BUSY      0x80U
#define AHTX0_STATUS_CAL       0x08U

#define AHTX0_POWER_ON_MS      100U
#define AHTX0_INIT_WAIT_MS     20U
#define AHTX0_MEASURE_WAIT_MS  100U
#define AHTX0_XFER_TIMEOUT_MS  120

static const char *TAG = "ahtx0";

static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static atomic_bool s_initialized;

static uint8_t ahtx0_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t ahtx0_add_device(uint8_t addr)
{
    if (s_dev && s_addr == addr)
        return ESP_OK;

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_bus_add_device(addr, AHT20_I2C_SPEED_HZ, &dev);
    if (err != ESP_OK)
        return err;

    s_dev = dev;
    s_addr = addr;
    return ESP_OK;
}

static esp_err_t ahtx0_probe_and_add(void)
{
    if (s_dev)
        return ESP_OK;

    const uint8_t addrs[] = { AHT20_I2C_ADDR, AHT20_I2C_ADDR_ALT };
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < sizeof(addrs); i++) {
        esp_err_t probe_err = i2c_bus_probe(addrs[i], AHTX0_XFER_TIMEOUT_MS);
        if (i == 0)
            first_err = probe_err;
        if (probe_err == ESP_OK) {
            esp_err_t add_err = ahtx0_add_device(addrs[i]);
            if (add_err == ESP_OK)
                ESP_LOGI(TAG, "detected at 0x%02x", addrs[i]);
            return add_err;
        }
        ESP_LOGW(TAG, "probe 0x%02x failed: %s", addrs[i],
                 esp_err_to_name(probe_err));
    }

    return first_err != ESP_OK ? first_err : ESP_ERR_NOT_FOUND;
}

static esp_err_t ahtx0_write_cmd(const uint8_t *cmd, size_t len)
{
    esp_err_t err = ahtx0_probe_and_add();
    if (err != ESP_OK)
        return err;
    return i2c_master_transmit(s_dev, cmd, len, AHTX0_XFER_TIMEOUT_MS);
}

static esp_err_t ahtx0_read_direct(uint8_t *data, size_t len)
{
    esp_err_t err = ahtx0_probe_and_add();
    if (err != ESP_OK)
        return err;
    return i2c_master_receive(s_dev, data, len, AHTX0_XFER_TIMEOUT_MS);
}

static esp_err_t ahtx0_read_with_status_cmd(uint8_t *data, size_t len)
{
    esp_err_t err = ahtx0_probe_and_add();
    if (err != ESP_OK)
        return err;

    const uint8_t cmd = AHTX0_CMD_STATUS;
    return i2c_master_transmit_receive(s_dev, &cmd, 1, data, len,
                                       AHTX0_XFER_TIMEOUT_MS);
}

static esp_err_t ahtx0_read_status(uint8_t *status)
{
    if (!status)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err = ahtx0_read_direct(status, 1);
    if (err == ESP_OK)
        return ESP_OK;

    esp_err_t cmd_err = ahtx0_read_with_status_cmd(status, 1);
    if (cmd_err == ESP_OK)
        return ESP_OK;

    ESP_LOGW(TAG, "status read failed: direct=%s 0x71=%s",
             esp_err_to_name(err), esp_err_to_name(cmd_err));
    return err;
}

static esp_err_t ahtx0_calibrate(uint8_t init_cmd)
{
    const uint8_t cmd[] = { init_cmd, 0x08, 0x00 };
    esp_err_t err = ahtx0_write_cmd(cmd, sizeof(cmd));
    if (err != ESP_OK)
        return err;

    vTaskDelay(pdMS_TO_TICKS(AHTX0_INIT_WAIT_MS));
    return ESP_OK;
}

esp_err_t aht20_init(void)
{
    if (atomic_load(&s_initialized))
        return ESP_OK;

    vTaskDelay(pdMS_TO_TICKS(AHTX0_POWER_ON_MS));

    esp_err_t err = ahtx0_probe_and_add();
    if (err != ESP_OK)
        return err;

    uint8_t status = 0;
    err = ahtx0_read_status(&status);
    if (err != ESP_OK)
        return err;

    if ((status & AHTX0_STATUS_CAL) == 0) {
        err = ahtx0_calibrate(AHTX0_CMD_INIT_AHT10);
        if (err != ESP_OK)
            return err;

        err = ahtx0_read_status(&status);
        if (err != ESP_OK)
            return err;

        if ((status & AHTX0_STATUS_CAL) == 0) {
            err = ahtx0_calibrate(AHTX0_CMD_INIT_AHT20);
            if (err != ESP_OK)
                return err;
            err = ahtx0_read_status(&status);
            if (err != ESP_OK)
                return err;
        }
    }

    if ((status & AHTX0_STATUS_CAL) == 0) {
        ESP_LOGW(TAG, "calibration bit not set, status=0x%02x", status);
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG, "ready at 0x%02x (status=0x%02x)", s_addr, status);
    return ESP_OK;
}

bool aht20_is_initialized(void)
{
    return atomic_load(&s_initialized);
}

uint8_t aht20_get_address(void)
{
    return s_addr ? s_addr : AHT20_I2C_ADDR;
}

esp_err_t aht20_probe(void)
{
    return ahtx0_probe_and_add();
}

esp_err_t aht20_soft_reset(void)
{
    const uint8_t cmd = AHTX0_CMD_SOFT_RESET;
    esp_err_t err = ahtx0_write_cmd(&cmd, 1);
    if (err != ESP_OK)
        return err;

    atomic_store(&s_initialized, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    return aht20_init();
}

static esp_err_t ahtx0_read_measurement_frame(uint8_t data[7], size_t *len)
{
    if (!data || !len)
        return ESP_ERR_INVALID_ARG;

    memset(data, 0, 7);

    esp_err_t err = ahtx0_read_direct(data, 6);
    if (err == ESP_OK) {
        *len = 6;
        return ESP_OK;
    }

    esp_err_t cmd_err = ahtx0_read_with_status_cmd(data, 7);
    if (cmd_err == ESP_OK) {
        *len = 7;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "measurement read failed: direct=%s 0x71=%s",
             esp_err_to_name(err), esp_err_to_name(cmd_err));
    return err;
}

esp_err_t aht20_read(aht20_sample_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    esp_err_t err = aht20_init();
    if (err != ESP_OK)
        return err;

    const uint8_t trigger_cmd[] = { AHTX0_CMD_TRIGGER, 0x33, 0x00 };
    err = ahtx0_write_cmd(trigger_cmd, sizeof(trigger_cmd));
    if (err != ESP_OK)
        return err;

    vTaskDelay(pdMS_TO_TICKS(AHTX0_MEASURE_WAIT_MS));

    uint8_t data[7] = {0};
    size_t data_len = 0;
    err = ahtx0_read_measurement_frame(data, &data_len);
    if (err != ESP_OK)
        return err;

    if (data[0] & AHTX0_STATUS_BUSY)
        return ESP_ERR_TIMEOUT;

    if (data_len == 7) {
        uint8_t crc = ahtx0_crc8(data, 6);
        if (crc != data[6]) {
            ESP_LOGW(TAG,
                     "crc mismatch: got 0x%02x expected 0x%02x frame=%02x %02x %02x %02x %02x %02x %02x",
                     data[6], crc, data[0], data[1], data[2], data[3],
                     data[4], data[5], data[6]);
            return ESP_ERR_INVALID_CRC;
        }
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
    out->calibrated = (data[0] & AHTX0_STATUS_CAL) != 0;
    out->status = data[0];
    out->i2c_addr = s_addr;
    out->humidity_percent = ((float)raw_h * 100.0f) / 1048576.0f;
    out->temperature_c = (((float)raw_t * 200.0f) / 1048576.0f) - 50.0f;

    if (out->humidity_percent < 0.0f)
        out->humidity_percent = 0.0f;
    if (out->humidity_percent > 100.0f)
        out->humidity_percent = 100.0f;
    return ESP_OK;
}
