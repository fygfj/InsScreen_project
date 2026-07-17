#include "sd_card.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

static SemaphoreHandle_t s_lock;
static bool s_initialized;
static bool s_vfs_registered;
static bool s_powered;
static bool s_mounted;
static bool s_dirs_ready;
static bool s_host_slot_inited;
static sdmmc_card_t *s_card;
static FATFS *s_fs;
static BYTE s_pdrv = 0xff;
static esp_err_t s_last_error = ESP_OK;
static esp_err_t s_last_dir_error = ESP_OK;

static SemaphoreHandle_t sd_card_lock(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    return s_lock;
}

static void sd_card_set_io_high_z(void)
{
    const gpio_num_t pins[] = {
        SD_CARD_CLK_GPIO,
        SD_CARD_CMD_GPIO,
        SD_CARD_D0_GPIO,
        GPIO_NUM_42, /* DAT2 is wired but unused in 1-bit mode. */
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        (void)gpio_set_direction(pins[i], GPIO_MODE_DISABLE);
        (void)gpio_set_pull_mode(pins[i], GPIO_FLOATING);
    }
}

static void sd_card_power_off_locked(void)
{
    sd_card_set_io_high_z();
    (void)gpio_set_level(SD_CARD_PWR_EN_GPIO, 1);
    s_powered = false;
}

static void sd_card_power_on_locked(void)
{
    (void)gpio_set_level(SD_CARD_PWR_EN_GPIO, 0);
    s_powered = true;
    vTaskDelay(pdMS_TO_TICKS(80));
}

static const char *sd_card_drive_path_locked(char out[3])
{
    out[0] = (char)('0' + s_pdrv);
    out[1] = ':';
    out[2] = 0;
    return out;
}

static esp_err_t sd_card_ensure_vfs_locked(void)
{
    if (s_vfs_registered)
        return ESP_OK;

    BYTE pdrv = 0xff;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK || pdrv == 0xff)
        return ESP_ERR_NO_MEM;

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    esp_vfs_fat_conf_t conf = {
        .base_path = SD_CARD_MOUNT_POINT,
        .fat_drive = drv,
        .max_files = 2,
    };
    err = esp_vfs_fat_register(&conf, &s_fs);
    if (err != ESP_OK)
        return err;

    s_pdrv = pdrv;
    s_vfs_registered = true;
    ESP_LOGI(TAG, "VFS registered: %s -> %s", SD_CARD_MOUNT_POINT, drv);
    return ESP_OK;
}

static sdmmc_slot_config_t sd_card_slot_config(void)
{
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SD_CARD_CLK_GPIO;
    slot_config.cmd = SD_CARD_CMD_GPIO;
    slot_config.d0 = SD_CARD_D0_GPIO;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.d4 = GPIO_NUM_NC;
    slot_config.d5 = GPIO_NUM_NC;
    slot_config.d6 = GPIO_NUM_NC;
    slot_config.d7 = GPIO_NUM_NC;
    slot_config.flags &= ~SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return slot_config;
}

static void sd_card_release_mount_locked(void)
{
    if (s_pdrv != 0xff) {
        char drv[3];
        (void)f_mount(NULL, sd_card_drive_path_locked(drv), 0);
        ff_diskio_unregister(s_pdrv);
    }
    if (s_host_slot_inited) {
        (void)sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
        s_host_slot_inited = false;
    }
    free(s_card);
    s_card = NULL;
    s_mounted = false;
    s_dirs_ready = false;
}

static esp_err_t sd_card_ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (mkdir(path, 0775) == 0)
        return ESP_OK;

    return errno == EEXIST ? ESP_OK : ESP_FAIL;
}

static esp_err_t sd_card_ensure_dirs_locked(void)
{
    const char *dirs[] = {
        SD_CARD_APP_DIR,
        SD_CARD_IMAGES_DIR,
        SD_CARD_BACKUP_DIR,
        SD_CARD_LOGS_DIR,
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        esp_err_t err = sd_card_ensure_dir(dirs[i]);
        if (err != ESP_OK) {
            s_dirs_ready = false;
            s_last_dir_error = err;
            ESP_LOGW(TAG, "directory not ready: %s (%s)",
                     dirs[i], esp_err_to_name(err));
            return err;
        }
    }

    s_dirs_ready = true;
    s_last_dir_error = ESP_OK;
    ESP_LOGI(TAG, "directories ready under %s", SD_CARD_APP_DIR);
    return ESP_OK;
}

static void sd_card_update_fs_info_locked(sd_card_status_t *st)
{
    if (!st || !s_mounted)
        return;

    struct statvfs vfs;
    if (statvfs(SD_CARD_MOUNT_POINT, &vfs) == 0) {
        st->total_bytes = (uint64_t)vfs.f_frsize * (uint64_t)vfs.f_blocks;
        st->free_bytes = (uint64_t)vfs.f_frsize * (uint64_t)vfs.f_bfree;
    }
}

esp_err_t sd_card_init(void)
{
    SemaphoreHandle_t lock = sd_card_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }

    gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << SD_CARD_PWR_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&pwr);
    if (err == ESP_OK) {
        (void)gpio_set_level(SD_CARD_PWR_EN_GPIO, 1);
        sd_card_set_io_high_z();
        s_powered = false;
        s_initialized = true;
        s_last_error = ESP_OK;
        ESP_LOGI(TAG, "ready: PWR_EN GPIO%d active-low, CLK GPIO%d, CMD GPIO%d, D0 GPIO%d",
                 (int)SD_CARD_PWR_EN_GPIO, (int)SD_CARD_CLK_GPIO,
                 (int)SD_CARD_CMD_GPIO, (int)SD_CARD_D0_GPIO);
    } else {
        s_last_error = err;
        ESP_LOGE(TAG, "power GPIO init failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(lock);
    return err;
}

esp_err_t sd_card_mount(void)
{
    esp_err_t err = sd_card_init();
    if (err != ESP_OK)
        return err;

    SemaphoreHandle_t lock = sd_card_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (s_mounted) {
        xSemaphoreGive(lock);
        return ESP_OK;
    }

    err = sd_card_ensure_vfs_locked();
    if (err != ESP_OK) {
        s_last_error = err;
        xSemaphoreGive(lock);
        return err;
    }

    sd_card_power_on_locked();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = sd_card_slot_config();
    s_card = calloc(1, sizeof(*s_card));
    if (!s_card) {
        err = ESP_ERR_NO_MEM;
        s_last_error = err;
        sd_card_power_off_locked();
        xSemaphoreGive(lock);
        return err;
    }

    ESP_LOGI(TAG, "mounting %s", SD_CARD_MOUNT_POINT);
    err = host.init();
    if (err == ESP_OK) {
        err = sdmmc_host_init_slot(host.slot, &slot_config);
        if (err == ESP_OK)
            s_host_slot_inited = true;
    }
    if (err == ESP_OK)
        err = sdmmc_card_init(&host, s_card);
    if (err == ESP_OK) {
        char drv[3];
        ff_diskio_register_sdmmc(s_pdrv, s_card);
        ff_sdmmc_set_disk_status_check(s_pdrv, false);
        FRESULT fr = f_mount(s_fs, sd_card_drive_path_locked(drv), 1);
        if (fr != FR_OK) {
            ESP_LOGW(TAG, "f_mount failed: %d", (int)fr);
            err = fr == FR_NOT_ENOUGH_CORE ? ESP_ERR_NO_MEM : ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        s_mounted = true;
        s_last_error = ESP_OK;
        (void)sd_card_ensure_dirs_locked();
        ESP_LOGI(TAG, "mounted: %s, capacity=%lluMB",
                 s_card && s_card->cid.name[0] ? s_card->cid.name : "SD",
                 s_card ? (unsigned long long)(s_card->csd.capacity *
                         (uint64_t)s_card->csd.sector_size / (1024ULL * 1024ULL)) : 0ULL);
    } else {
        s_mounted = false;
        s_last_error = err;
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(err));
        sd_card_release_mount_locked();
        sd_card_power_off_locked();
    }

    xSemaphoreGive(lock);
    return err;
}

esp_err_t sd_card_unmount(void)
{
    SemaphoreHandle_t lock = sd_card_lock();
    if (!lock)
        return ESP_ERR_NO_MEM;

    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (s_mounted) {
        char drv[3];
        FRESULT fr = f_mount(NULL, sd_card_drive_path_locked(drv), 0);
        if (fr == FR_OK) {
            ESP_LOGI(TAG, "unmounted");
        } else {
            err = ESP_FAIL;
            s_last_error = err;
            ESP_LOGW(TAG, "f_mount unmount failed: %d", (int)fr);
        }
    }

    sd_card_release_mount_locked();
    sd_card_power_off_locked();
    if (err == ESP_OK)
        s_last_error = ESP_OK;

    xSemaphoreGive(lock);
    return err;
}

esp_err_t sd_card_prepare_sleep(void)
{
    return sd_card_unmount();
}

void sd_card_get_status(sd_card_status_t *out)
{
    if (!out)
        return;

    memset(out, 0, sizeof(*out));

    SemaphoreHandle_t lock = sd_card_lock();
    if (!lock) {
        out->last_error = ESP_ERR_NO_MEM;
        return;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    out->initialized = s_initialized;
    out->powered = s_powered;
    out->mounted = s_mounted;
    out->card_present = s_mounted;
    out->dirs_ready = s_mounted && s_dirs_ready;
    out->last_error = s_last_error;
    out->last_dir_error = s_last_dir_error;

    if (s_card) {
        snprintf(out->card_name, sizeof(out->card_name), "%s",
                 s_card->cid.name[0] ? s_card->cid.name : "SD");
        out->sector_size = s_card->csd.sector_size;
        out->capacity_mb = (uint32_t)(s_card->csd.capacity *
                                      (uint64_t)s_card->csd.sector_size /
                                      (1024ULL * 1024ULL));
    }
    sd_card_update_fs_info_locked(out);
    xSemaphoreGive(lock);
}

const char *sd_card_mount_point(void)
{
    return SD_CARD_MOUNT_POINT;
}
